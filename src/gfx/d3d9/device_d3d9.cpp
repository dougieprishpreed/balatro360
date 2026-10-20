// Direct3D 9 backend for the Xbox 360 (XDK).
//
// Notes on the 360 specifics handled here:
//  * Render targets live in EDRAM. The back buffer occupies the start of
//    EDRAM; every canvas uses one shared EDRAM surface placed right after it
//    and is Resolve()d into its own texture whenever we switch away from it.
//    If a canvas is re-bound and drawn to without a clear, its previous
//    contents are copied back into EDRAM first.
//  * CPU-filled textures use linear (LIN_) formats so no tiling is needed.
//    Texel values are written as 0xAARRGGBB DWORDs (native endian).
//  * Shaders are translated from LÖVE GLSL to HLSL at load time
//    (shader_translate.cpp) and compiled on the console with
//    D3DXCompileShader. Uniforms are set through the D3DX constant tables.
#include <xtl.h>
#include <xgraphics.h>
#include <d3dx9.h>

#include "../device.h"
#include "../shader_translate.h"
#include "../../platform/platform.h"

#include <map>
#include <string.h>

// Set to 0 if your XDK's d3d9 headers have no D3DRS_SEPARATEALPHABLENDENABLE
// (alpha blend states are then always separate).
#ifndef B360_HAS_SEPARATEALPHABLENDENABLE
#define B360_HAS_SEPARATEALPHABLENDENABLE 1
#endif

// The 360's Direct3D can change the presentation interval at any time with
// D3DRS_PRESENTINTERVAL. Set to 0 if your XDK lacks it; frames are then
// paced with timed waits instead.
#ifndef B360_HAS_PRESENTINTERVAL_RS
#define B360_HAS_PRESENTINTERVAL_RS 1
#endif

namespace gfx {

namespace {

IDirect3DDevice9* g_device = NULL;

class XeTexture : public Texture {
public:
    IDirect3DTexture9* tex;
    bool linear;        // LIN_ format (CPU writable)
    bool usedByGPU;     // drawn with since the last CPU update
    bool needsRestore;  // canvas: EDRAM must be refilled before drawing
    XeTexture() : tex(NULL), linear(true), usedByGPU(false), needsRestore(false) {}
};

// Where one uniform lives in one stage's constant registers.
struct ConstLoc {
    int reg;             // first float4 register, -1 if the stage doesn't use it
    int regs;            // registers it occupies
    D3DXHANDLE handle;   // only for uniforms that need D3DX packing (matrices)
    ConstLoc() : reg(-1), regs(0), handle(NULL) {}
};

void findConst(ID3DXConstantTable* table, const std::string& name, ConstLoc& loc, bool& packed) {
    if (!table) return;
    D3DXHANDLE h = table->GetConstantByName(NULL, name.c_str());
    if (!h) h = table->GetConstantByName(NULL, (name + "_").c_str());   // renamed HLSL keyword
    if (!h) return;
    D3DXCONSTANT_DESC d;
    UINT n = 1;
    if (FAILED(table->GetConstantDesc(h, &d, &n)) || n < 1) return;
    if (d.RegisterSet == D3DXRS_FLOAT4 && (d.Class == D3DXPC_SCALAR || d.Class == D3DXPC_VECTOR)) {
        loc.reg = (int)d.RegisterIndex;
        loc.regs = (int)d.RegisterCount;
    } else {
        loc.handle = h;
        packed = true;
    }
}

// 16-byte aligned float4 registers for Set*ShaderConstantF.
inline float* align16(float* p) { return (float*)(((size_t)p + 15) & ~(size_t)15); }

struct UniformSlot {
    ConstLoc vs, ps;
    bool packed;               // set through D3DX (matrices and other odd types)
    int elemFloats;            // floats per array element as the core packs them
    int regs;                  // float4 registers held in raw
    std::vector<float> raw;    // registers (aligned via regData()); packed: the values
    bool dirty;
    UniformSlot() : packed(false), elemFloats(4), regs(0), dirty(false) {}
    float* regData() { return align16(&raw[0]); }
};

class XeShader : public Shader {
public:
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
    ID3DXConstantTable* vsConst;
    ID3DXConstantTable* psConst;
    std::vector<UniformSlot> slots;   // parallel to uniforms
    ConstLoc projVS, screenVS, screenPS;
    bool anyDirty;

    XeShader() : vs(NULL), ps(NULL), vsConst(NULL), psConst(NULL), anyDirty(false) {}
    ~XeShader() {
        if (vs) vs->Release();
        if (ps) ps->Release();
        if (vsConst) vsConst->Release();
        if (psConst) psConst->Release();
    }

    void resolveConstants() {
        bool dummy = false;
        findConst(vsConst, "love_ProjRows", projVS, dummy);
        findConst(vsConst, "love_ScreenSize", screenVS, dummy);
        findConst(psConst, "love_ScreenSize", screenPS, dummy);
        slots.resize(uniforms.size());
        for (size_t i = 0; i < uniforms.size(); ++i) {
            const UniformInfo& u = uniforms[i];
            UniformSlot& sl = slots[i];
            if (u.type == UNIFORM_SAMPLER) continue;
            findConst(vsConst, u.name, sl.vs, sl.packed);
            findConst(psConst, u.name, sl.ps, sl.packed);
            sl.elemFloats = u.type == UNIFORM_MAT ? u.components * u.components : u.components;
            if (sl.packed) continue;
            sl.regs = sl.vs.regs > sl.ps.regs ? sl.vs.regs : sl.ps.regs;
            if (sl.regs > 0) sl.raw.assign(sl.regs * 4 + 3, 0.0f);
        }
    }

    virtual void setUniform(int index, const float* v, int count) {
        if (index < 0 || index >= (int)slots.size()) return;
        UniformSlot& sl = slots[index];
        if (sl.packed) {
            sl.raw.assign(v, v + count);
        } else if (sl.regs > 0) {
            // One array element per float4 register (SM3 never packs
            // separate scalars into one register).
            float* d = sl.regData();
            int per = sl.elemFloats < 4 ? sl.elemFloats : 4;
            for (int e = 0; e < sl.regs && e * sl.elemFloats < count; ++e)
                for (int c = 0; c < per && e * sl.elemFloats + c < count; ++c)
                    d[e * 4 + c] = v[e * sl.elemFloats + c];
        }
        sl.dirty = true;
        anyDirty = true;
    }
    virtual void setUniformTexture(int index, Texture* tex) {
        (void)index;
        (void)tex;   // Balatro only samples the main texture.
    }

    static void uploadLoc(const ConstLoc& loc, bool vertex, const float* data) {
        if (loc.reg < 0) return;
        if (vertex) g_device->SetVertexShaderConstantF(loc.reg, data, loc.regs);
        else g_device->SetPixelShaderConstantF(loc.reg, data, loc.regs);
    }

    void uploadSlot(UniformSlot& sl) {
        sl.dirty = false;
        if (sl.raw.empty()) return;
        if (sl.packed) {
            if (sl.vs.handle) vsConst->SetFloatArray(g_device, sl.vs.handle, &sl.raw[0], (UINT)sl.raw.size());
            if (sl.ps.handle) psConst->SetFloatArray(g_device, sl.ps.handle, &sl.raw[0], (UINT)sl.raw.size());
            return;
        }
        uploadLoc(sl.vs, true, sl.regData());
        uploadLoc(sl.ps, false, sl.regData());
    }

    // all: the registers hold another shader's values.
    void upload(bool all) {
        if (!all && !anyDirty) return;
        for (size_t i = 0; i < slots.size(); ++i)
            if (all || slots[i].dirty) uploadSlot(slots[i]);
        anyDirty = false;
    }

    void uploadBuiltins(const float* projRows, const float* screen) {
        if (projVS.reg >= 0) g_device->SetVertexShaderConstantF(projVS.reg, projRows, 4);
        if (screenVS.reg >= 0) g_device->SetVertexShaderConstantF(screenVS.reg, screen, 1);
        if (screenPS.reg >= 0) g_device->SetPixelShaderConstantF(screenPS.reg, screen, 1);
    }
};

bool compileStage(const std::string& src, bool vertex, ID3DXBuffer** code, ID3DXConstantTable** table,
                  std::string& err) {
    ID3DXBuffer* errors = NULL;
    HRESULT hr = D3DXCompileShader(src.c_str(), (UINT)src.size(), NULL, NULL, "love_main",
                                   vertex ? "vs_3_0" : "ps_3_0", 0, code, &errors, table);
    if (FAILED(hr)) {
        err += vertex ? "vertex: " : "pixel: ";
        if (errors) err += (const char*)errors->GetBufferPointer();
        else err += "D3DXCompileShader failed";
        err += "\n";
        if (errors) errors->Release();
        return false;
    }
    if (errors) errors->Release();
    return true;
}

DWORD argb(const u8* p) {
    return ((DWORD)p[3] << 24) | ((DWORD)p[0] << 16) | ((DWORD)p[1] << 8) | (DWORD)p[2];
}

class XeDevice : public Device {
public:
    XeDevice()
        : d3d(NULL), w(0), h(0), backbuffer(NULL), canvasSurface(NULL), canvasW(0), canvasH(0),
          edramCanvasBase(0), current(NULL), dirtyCanvas(false), decl(NULL), defaultShader(NULL),
          boundShader(NULL), constOwner(NULL), constW(0), constH(0), boundTex(NULL), blend(-1),
          presentInterval(1), needDecl(true) {}

    virtual bool init(int width, int height) {
        w = width;
        h = height;
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d) return false;
        D3DPRESENT_PARAMETERS pp;
        memset(&pp, 0, sizeof(pp));
        pp.BackBufferWidth = w;
        pp.BackBufferHeight = h;
        pp.BackBufferFormat = D3DFMT_A8R8G8B8;
        pp.BackBufferCount = 1;
        pp.MultiSampleType = D3DMULTISAMPLE_NONE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.EnableAutoDepthStencil = FALSE;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_device))) {
            plat::log("[d3d] CreateDevice failed");
            return false;
        }
        g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
        edramCanvasBase = XGSurfaceSize(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE);

        D3DVERTEXELEMENT9 elems[] = {
            {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            D3DDECL_END()};
        if (FAILED(g_device->CreateVertexDeclaration(elems, &decl))) return false;

        g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        g_device->SetRenderState(D3DRS_HALFPIXELOFFSET, TRUE);   // GL-style pixel centres

        std::string err;
        defaultShader = (XeShader*)createShader("", err);
        if (!defaultShader) {
            plat::log("[d3d] default shader failed:\n%s", err.c_str());
            return false;
        }
        setRenderTarget(NULL);
        return true;
    }

    virtual int width() const { return w; }
    virtual int height() const { return h; }

    virtual Texture* createTexture(int tw, int th, const u8* pixels, bool mipmaps) {
        (void)mipmaps;   // TODO: mipmaps for linear textures
        XeTexture* t = new XeTexture;
        t->width = tw;
        t->height = th;
        if (FAILED(g_device->CreateTexture(tw, th, 1, 0, D3DFMT_LIN_A8R8G8B8, 0, &t->tex, NULL))) {
            plat::log("[d3d] CreateTexture %dx%d failed", tw, th);
            delete t;
            return NULL;
        }
        if (pixels) writePixels(t, 0, 0, tw, th, pixels);
        return t;
    }

    virtual Texture* createRenderTarget(int tw, int th) {
        XeTexture* t = new XeTexture;
        t->width = tw;
        t->height = th;
        t->renderTarget = true;
        t->linear = false;
        if (FAILED(g_device->CreateTexture(tw, th, 1, 0, D3DFMT_A8R8G8B8, 0, &t->tex, NULL))) {
            plat::log("[d3d] CreateTexture (canvas) %dx%d failed", tw, th);
            delete t;
            return NULL;
        }
        if (!ensureCanvasSurface(tw, th)) {
            t->tex->Release();
            delete t;
            return NULL;
        }
        return t;
    }

    virtual void updateTexture(Texture* tex, int x, int y, int tw, int th, const u8* pixels) {
        writePixels((XeTexture*)tex, x, y, tw, th, pixels);
    }

    virtual void destroyTexture(Texture* tex) {
        XeTexture* t = (XeTexture*)tex;
        if (!t) return;
        if (current == t) setRenderTarget(NULL);
        if (boundTex == t) {
            g_device->SetTexture(0, NULL);
            boundTex = NULL;
        }
        waitGPU(t);
        t->tex->Release();
        delete t;
    }

    virtual void setTextureParams(Texture* tex, Filter minF, Filter magF, Wrap ws, Wrap wt) {
        tex->minFilter = minF;
        tex->magFilter = magF;
        tex->wrapS = ws;
        tex->wrapT = wt;
        if (boundTex == tex) boundTex = NULL;   // re-apply sampler state
    }

    virtual Shader* createShader(const std::string& source, std::string& err) {
        double t0 = plat::getTime();
        Shader* s = compileShader(source, err);
        if (s) plat::log("[d3d] shader compiled in %.0f ms", (plat::getTime() - t0) * 1000.0);
        return s;
    }

    Shader* compileShader(const std::string& source, std::string& err) {
        LoveShaderInfo info;
        analyzeLoveShader(source, info);
        std::vector<std::string> warnings;
        std::string vsrc = buildHLSLStage(source, info, true, &warnings);
        std::string psrc = buildHLSLStage(source, info, false, &warnings);
        for (size_t i = 0; i < warnings.size(); ++i) plat::log("[d3d] shader warning: %s", warnings[i].c_str());

        ID3DXBuffer *vcode = NULL, *pcode = NULL;
        ID3DXConstantTable *vtab = NULL, *ptab = NULL;
        bool ok = compileStage(vsrc, true, &vcode, &vtab, err);
        ok = compileStage(psrc, false, &pcode, &ptab, err) && ok;
        if (!ok) {
            plat::log("[d3d] shader compile failed:\n%s", err.c_str());
            if (vcode) vcode->Release();
            if (pcode) pcode->Release();
            if (vtab) vtab->Release();
            if (ptab) ptab->Release();
            return NULL;
        }
        XeShader* s = new XeShader;
        s->uniforms = info.uniforms;
        s->vsConst = vtab;
        s->psConst = ptab;
        s->resolveConstants();
        HRESULT hr1 = g_device->CreateVertexShader((const DWORD*)vcode->GetBufferPointer(), &s->vs);
        HRESULT hr2 = g_device->CreatePixelShader((const DWORD*)pcode->GetBufferPointer(), &s->ps);
        vcode->Release();
        pcode->Release();
        if (FAILED(hr1) || FAILED(hr2)) {
            err += "CreateVertexShader/CreatePixelShader failed";
            delete s;
            return NULL;
        }
        return s;
    }

    virtual void destroyShader(Shader* sh) {
        if (constOwner == sh) constOwner = NULL;
        if (boundShader == sh) {
            boundShader = NULL;
            g_device->SetVertexShader(NULL);
            g_device->SetPixelShader(NULL);
        }
        delete (XeShader*)sh;
    }

    virtual void setRenderTarget(Texture* rt) {
        XeTexture* t = (XeTexture*)rt;
        if (current && current == t && rt) return;
        resolveCurrent();
        current = t;
        if (t) {
            g_device->SetRenderTarget(0, canvasSurface);
            t->needsRestore = true;
        } else {
            g_device->SetRenderTarget(0, backbuffer);
        }
        D3DVIEWPORT9 vp;
        vp.X = 0;
        vp.Y = 0;
        vp.Width = t ? t->width : w;
        vp.Height = t ? t->height : h;
        vp.MinZ = 0;
        vp.MaxZ = 1;
        g_device->SetViewport(&vp);
        dirtyCanvas = false;
    }

    virtual void clear(float r, float g, float b, float a) {
        if (current) current->needsRestore = false;
        g_device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(r, g, b, a), 1.0f, 0);
        dirtyCanvas = true;
        afterInternalDraw();
    }

    virtual void setScissor(bool enabled, int x, int y, int sw, int sh) {
        g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, enabled ? TRUE : FALSE);
        if (enabled) {
            RECT rc;
            rc.left = x;
            rc.top = y;
            rc.right = x + sw;
            rc.bottom = y + sh;
            g_device->SetScissorRect(&rc);
        }
    }

    virtual void setColorMask(bool r, bool g, bool b, bool a) {
        DWORD m = (r ? D3DCOLORWRITEENABLE_RED : 0) | (g ? D3DCOLORWRITEENABLE_GREEN : 0) |
                  (b ? D3DCOLORWRITEENABLE_BLUE : 0) | (a ? D3DCOLORWRITEENABLE_ALPHA : 0);
        g_device->SetRenderState(D3DRS_COLORWRITEENABLE, m);
    }

    virtual void drawTriangles(const Vertex* verts, int count, Texture* tex, Shader* shader, BlendMode b) {
        if (count <= 0) return;
        if (current && current->needsRestore) restoreCanvas();
        XeShader* s = shader ? (XeShader*)shader : defaultShader;
        bindShader(s);
        bindTexture((XeTexture*)tex);
        applyBlend(b);
        if (needDecl) {
            g_device->SetVertexDeclaration(decl);
            needDecl = false;
        }
        // DrawPrimitiveUP copies into the command ring buffer; keep each call
        // modest in size.
        const int maxVerts = 3 * 2048;
        for (int start = 0; start < count; start += maxVerts) {
            int n = count - start;
            if (n > maxVerts) n = maxVerts;
            g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, verts + start, sizeof(Vertex));
        }
        ((XeTexture*)tex)->usedByGPU = true;
        dirtyCanvas = true;
    }

    virtual void present() {
        if (current) setRenderTarget(NULL);
        g_device->Present(NULL, NULL, NULL, NULL);
        afterInternalDraw();
    }

    virtual bool setPresentInterval(int n) {
#if B360_HAS_PRESENTINTERVAL_RS
        n = n >= 2 ? 2 : 1;
        if (n != presentInterval) {
            presentInterval = n;
            g_device->SetRenderState(D3DRS_PRESENTINTERVAL,
                                     n == 2 ? D3DPRESENT_INTERVAL_TWO : D3DPRESENT_INTERVAL_ONE);
        }
        return true;
#else
        (void)n;
        return false;
#endif
    }

    virtual const char* rendererName() const { return "Direct3D 9 (Xbox 360)"; }

private:
    IDirect3D9* d3d;
    int w, h;
    IDirect3DSurface9* backbuffer;
    IDirect3DSurface9* canvasSurface;
    int canvasW, canvasH;
    DWORD edramCanvasBase;
    XeTexture* current;
    bool dirtyCanvas;
    IDirect3DVertexDeclaration9* decl;
    XeShader* defaultShader;
    XeShader* boundShader;
    XeShader* constOwner;   // whose uniforms the constant registers hold
    int constW, constH;     // target size the built-in constants were set for
    XeTexture* boundTex;
    int blend;
    int presentInterval;
    bool needDecl;          // vertex declaration must be (re)set before drawing

    // Clear, Resolve and Present may be implemented with internal draws on
    // the 360. Re-send the vertex declaration and all shader constants after
    // them rather than trusting they survived (cheap: a few times a frame).
    void afterInternalDraw() {
        constOwner = NULL;
        needDecl = true;
    }

    bool ensureCanvasSurface(int tw, int th) {
        if (canvasSurface && tw <= canvasW && th <= canvasH) return true;
        if (canvasSurface) {
            if (current) setRenderTarget(NULL);
            canvasSurface->Release();
            canvasSurface = NULL;
        }
        int nw = tw > w ? tw : w, nh = th > h ? th : h;
        D3DSURFACE_PARAMETERS sp;
        memset(&sp, 0, sizeof(sp));
        sp.Base = edramCanvasBase;
        if (FAILED(g_device->CreateRenderTarget(nw, nh, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                &canvasSurface, &sp))) {
            plat::log("[d3d] CreateRenderTarget %dx%d failed (EDRAM full?)", nw, nh);
            return false;
        }
        canvasW = nw;
        canvasH = nh;
        return true;
    }

    // Copy the bound canvas's EDRAM contents into its texture.
    void resolveCurrent() {
        if (!current || !dirtyCanvas) return;
        D3DRECT src;
        src.x1 = 0;
        src.y1 = 0;
        src.x2 = current->width;
        src.y2 = current->height;
        g_device->Resolve(D3DRESOLVE_RENDERTARGET0, &src, current->tex, NULL, 0, 0, NULL, 0.0f, 0, NULL);
        dirtyCanvas = false;
        afterInternalDraw();
    }

    // Draw the canvas texture back into EDRAM (the game drew to a canvas
    // that was bound before without clearing it first).
    void restoreCanvas() {
        XeTexture* t = current;
        t->needsRestore = false;
        Vertex v[6];
        float x1 = (float)t->width, y1 = (float)t->height;
        float pos[6][4] = {{0, 0, 0, 0}, {x1, 0, 1, 0}, {0, y1, 0, 1}, {x1, 0, 1, 0}, {x1, y1, 1, 1}, {0, y1, 0, 1}};
        for (int i = 0; i < 6; ++i) {
            v[i].x = pos[i][0];
            v[i].y = pos[i][1];
            v[i].u = pos[i][2];
            v[i].v = pos[i][3];
            v[i].r = v[i].g = v[i].b = v[i].a = 1.0f;
        }
        Filter mi = t->minFilter, ma = t->magFilter;
        t->minFilter = t->magFilter = FILTER_NEAREST;
        boundTex = NULL;
        bindShader(defaultShader);
        bindTexture(t);
        applyBlend(BLEND_REPLACE);
        if (needDecl) {
            g_device->SetVertexDeclaration(decl);
            needDecl = false;
        }
        g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(Vertex));
        t->minFilter = mi;
        t->magFilter = ma;
        boundTex = NULL;
        blend = -1;
    }

    void waitGPU(XeTexture* t) {
        if (!t->usedByGPU) return;
        DWORD fence = g_device->InsertFence();
        g_device->BlockOnFence(fence);
        t->usedByGPU = false;
    }

    void writePixels(XeTexture* t, int x, int y, int tw, int th, const u8* pixels) {
        if (!t->linear) return;
        waitGPU(t);
        RECT rc;
        rc.left = x;
        rc.top = y;
        rc.right = x + tw;
        rc.bottom = y + th;
        D3DLOCKED_RECT lr;
        if (FAILED(t->tex->LockRect(0, &lr, &rc, 0))) return;
        for (int row = 0; row < th; ++row) {
            DWORD* dst = (DWORD*)((u8*)lr.pBits + row * lr.Pitch);
            const u8* src = pixels + (size_t)row * tw * 4;
            for (int col = 0; col < tw; ++col) dst[col] = argb(src + col * 4);
        }
        t->tex->UnlockRect(0);
    }

    void bindShader(XeShader* s) {
        int tw = current ? current->width : w;
        int th = current ? current->height : h;
        if (boundShader != s) {
            g_device->SetVertexShader(s->vs);
            g_device->SetPixelShader(s->ps);
            boundShader = s;
        }
        // Constants are device state shared by all shaders: after another
        // shader drew, everything is re-uploaded; otherwise only what changed.
        bool all = constOwner != s;
        if (all || constW != tw || constH != th) {
            // Pixels (y down) -> clip space (y up), then love_ScreenSize.
            float buf[20 + 3];
            float* c = align16(buf);
            const float values[20] = {
                2.0f / tw, 0, 0, -1,
                0, -2.0f / th, 0, 1,
                0, 0, 1, 0,
                0, 0, 0, 1,
                (float)tw, (float)th, 1.0f, 0.0f};
            memcpy(c, values, sizeof(values));
            s->uploadBuiltins(c, c + 16);
            constW = tw;
            constH = th;
        }
        s->upload(all);
        constOwner = s;
    }

    void bindTexture(XeTexture* t) {
        if (boundTex == t) return;
        g_device->SetTexture(0, t->tex);
        DWORD minF = t->minFilter == FILTER_NEAREST ? D3DTEXF_POINT : D3DTEXF_LINEAR;
        DWORD magF = t->magFilter == FILTER_NEAREST ? D3DTEXF_POINT : D3DTEXF_LINEAR;
        g_device->SetSamplerState(0, D3DSAMP_MINFILTER, minF);
        g_device->SetSamplerState(0, D3DSAMP_MAGFILTER, magF);
        g_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        g_device->SetSamplerState(0, D3DSAMP_ADDRESSU, address(t->wrapS));
        g_device->SetSamplerState(0, D3DSAMP_ADDRESSV, address(t->wrapT));
        boundTex = t;
    }

    static DWORD address(Wrap wr) {
        if (wr == WRAP_REPEAT) return D3DTADDRESS_WRAP;
        if (wr == WRAP_MIRROR) return D3DTADDRESS_MIRROR;
        return D3DTADDRESS_CLAMP;
    }

    void setBlend(DWORD op, DWORD src, DWORD dst, DWORD opA, DWORD srcA, DWORD dstA) {
        g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
#if B360_HAS_SEPARATEALPHABLENDENABLE
        g_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
#endif
        g_device->SetRenderState(D3DRS_BLENDOP, op);
        g_device->SetRenderState(D3DRS_SRCBLEND, src);
        g_device->SetRenderState(D3DRS_DESTBLEND, dst);
        g_device->SetRenderState(D3DRS_BLENDOPALPHA, opA);
        g_device->SetRenderState(D3DRS_SRCBLENDALPHA, srcA);
        g_device->SetRenderState(D3DRS_DESTBLENDALPHA, dstA);
    }

    void applyBlend(BlendMode b) {
        if ((int)b == blend) return;
        blend = (int)b;
        switch (b) {
        case BLEND_ALPHA:
            setBlend(D3DBLENDOP_ADD, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, D3DBLEND_ONE,
                     D3DBLEND_INVSRCALPHA);
            break;
        case BLEND_ALPHA_PREMUL:
            setBlend(D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, D3DBLEND_ONE,
                     D3DBLEND_INVSRCALPHA);
            break;
        case BLEND_ADD:
            setBlend(D3DBLENDOP_ADD, D3DBLEND_SRCALPHA, D3DBLEND_ONE, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ONE);
            break;
        case BLEND_SUBTRACT:
            setBlend(D3DBLENDOP_REVSUBTRACT, D3DBLEND_SRCALPHA, D3DBLEND_ONE, D3DBLENDOP_REVSUBTRACT,
                     D3DBLEND_ZERO, D3DBLEND_ONE);
            break;
        case BLEND_MULTIPLY:
            setBlend(D3DBLENDOP_ADD, D3DBLEND_DESTCOLOR, D3DBLEND_ZERO, D3DBLENDOP_ADD, D3DBLEND_DESTALPHA,
                     D3DBLEND_ZERO);
            break;
        case BLEND_SCREEN:
            setBlend(D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_INVSRCCOLOR, D3DBLENDOP_ADD, D3DBLEND_ONE,
                     D3DBLEND_INVSRCCOLOR);
            break;
        case BLEND_REPLACE:
        default:
            g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            break;
        }
    }
};

}  // namespace

Device* createDevice() { return new XeDevice; }

}  // namespace gfx
