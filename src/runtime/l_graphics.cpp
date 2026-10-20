// love.graphics
//
// Immediate-mode 2D renderer: every draw call is transformed on the CPU and
// appended to a vertex batch that is flushed to the gfx::Device whenever
// the texture, shader, blend mode or render target changes.
#include "modules.h"
#include "font.h"
#include "filesystem.h"
#include "pacing.h"
#include "../gfx/device.h"
#include "../gfx/shader_translate.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <math.h>
#include <string.h>
#include <map>

namespace rt {

namespace {

// -------------------------------------------------------------- math types
struct Matrix {
    // | a c e |
    // | b d f |
    float a, b, c, d, e, f;
    Matrix() : a(1), b(0), c(0), d(1), e(0), f(0) {}
    void apply(float x, float y, float& ox, float& oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
    Matrix operator*(const Matrix& m) const {
        Matrix r;
        r.a = a * m.a + c * m.b;
        r.b = b * m.a + d * m.b;
        r.c = a * m.c + c * m.d;
        r.d = b * m.c + d * m.d;
        r.e = a * m.e + c * m.f + e;
        r.f = b * m.e + d * m.f + f;
        return r;
    }
    // LÖVE's Matrix4::setTransformation.
    static Matrix transformation(float x, float y, float angle, float sx, float sy, float ox, float oy,
                                 float kx, float ky) {
        Matrix m;
        float co = cosf(angle), si = sinf(angle);
        m.a = co * sx - ky * si * sy;
        m.b = si * sx + ky * co * sy;
        m.c = kx * co * sx - si * sy;
        m.d = kx * si * sx + co * sy;
        m.e = x - ox * m.a - oy * m.c;
        m.f = y - ox * m.b - oy * m.d;
        return m;
    }
    bool invert(Matrix& out) const {
        float det = a * d - b * c;
        if (det == 0) return false;
        float id = 1.0f / det;
        out.a = d * id;
        out.b = -b * id;
        out.c = -c * id;
        out.d = a * id;
        out.e = (c * f - d * e) * id;
        out.f = (b * e - a * f) * id;
        return true;
    }
};

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// --------------------------------------------------------------- objects
gfx::Device* g_dev = NULL;

class TextureObject : public Object {
public:
    gfx::Texture* tex;
    int pw, ph;
    float dpi;
    bool mipmaps;
    TextureObject() : tex(NULL), pw(0), ph(0), dpi(1), mipmaps(false) {}
    ~TextureObject() {
        if (tex && g_dev) g_dev->destroyTexture(tex);
    }
    float width() const { return pw / dpi; }
    float height() const { return ph / dpi; }
};

class Image : public TextureObject {};
class Canvas : public TextureObject {};

class Quad : public Object {
public:
    double x, y, w, h, sw, sh;
};

class ShaderObject : public Object {
public:
    gfx::Shader* sh;
    // Last value sent to each uniform (indexed like sh->uniforms). Balatro
    // re-sends a dozen uniforms before every sprite it draws, mostly with
    // unchanged values; those sends then cost nothing.
    std::vector<std::vector<float> > values;
    ShaderObject() : sh(NULL) {}
    ~ShaderObject() {
        if (sh && g_dev) g_dev->destroyShader(sh);
    }
    int find(const char* name) const {
        for (size_t i = 0; i < sh->uniforms.size(); ++i)
            if (strcmp(sh->uniforms[i].name.c_str(), name) == 0) return (int)i;
        return -1;
    }
};

// Float array that stays on the stack for typical uniform sizes.
struct FloatBuf {
    float local[64];
    std::vector<float> heap;
    int n;
    FloatBuf() : n(0) {}
    void push(float v) {
        if (n < 64) {
            local[n] = v;
        } else {
            if (n == 64) heap.assign(local, local + 64);
            heap.push_back(v);
        }
        ++n;
    }
    const float* data() const { return n <= 64 ? local : &heap[0]; }
};

class TextObject : public Object {
public:
    Ref<Font> font;
    struct Entry {
        std::vector<ColoredString> text;
        float limit;
        AlignMode align;
        Matrix m;
    };
    std::vector<Entry> entries;
    std::vector<GlyphQuad> quads;
    float width, height;
    bool dirty;
    TextObject() : width(0), height(0), dirty(true) {}

    void rebuild() {
        if (!dirty) return;
        dirty = false;
        quads.clear();
        width = height = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            std::vector<GlyphQuad> q;
            float w = 0;
            int lines = font->layout(entries[i].text, entries[i].limit, entries[i].align, q, &w);
            float h = lines * (float)floor(font->getHeight() * font->getLineHeight() + 0.5f);
            const Matrix& m = entries[i].m;
            for (size_t k = 0; k < q.size(); ++k) {
                // Entries added with add() carry their own transform; we only
                // support translation/scale here (sufficient for LÖVE usage).
                GlyphQuad g = q[k];
                m.apply(q[k].x0, q[k].y0, g.x0, g.y0);
                m.apply(q[k].x1, q[k].y1, g.x1, g.y1);
                quads.push_back(g);
            }
            float right = m.e + w * m.a, bottom = m.f + h * m.d;
            if (right > width) width = right;
            if (bottom > height) height = bottom;
        }
    }
};

// ----------------------------------------------------------------- state
enum LineJoin { JOIN_MITER = 0, JOIN_BEVEL, JOIN_NONE };

struct GState {
    ColorF color;
    ColorF bg;
    Ref<Font> font;
    Ref<ShaderObject> shader;
    Ref<Canvas> canvas;
    gfx::BlendMode blend;
    float lineWidth;
    LineJoin lineJoin;
    bool roughLines;
    float pointSize;
    bool scissor;
    int sx, sy, sw, sh;
    bool mask[4];
};

struct StackEntry {
    Matrix m;
    bool hasState;
    GState state;
};

GState g_state;
Matrix g_transform;
std::vector<StackEntry> g_stack;
gfx::Filter g_defaultMin = gfx::FILTER_LINEAR, g_defaultMag = gfx::FILTER_LINEAR;
gfx::Texture* g_white = NULL;
Ref<Font> g_defaultFont;

// Batch: the first g_batchCount entries of g_batch are pending vertices
// (the vector only ever grows, so appending never reallocates per vertex).
std::vector<gfx::Vertex> g_batch;
int g_batchCount = 0;
gfx::Texture* g_batchTex = NULL;
int g_drawCalls = 0;
int g_drawCallsLast = 0;
int g_canvasSwitches = 0;
int g_canvasSwitchesLast = 0;
int g_vertices = 0;
int g_verticesLast = 0;
FramePacer g_pacer;

// Render target currently bound on the device.
Canvas* g_appliedCanvas = NULL;

void flush() {
    if (g_batchCount == 0) return;
    gfx::Shader* sh = g_state.shader.get() ? g_state.shader->sh : NULL;
    g_dev->drawTriangles(&g_batch[0], g_batchCount, g_batchTex ? g_batchTex : g_white, sh, g_state.blend);
    g_vertices += g_batchCount;
    g_batchCount = 0;
    ++g_drawCalls;
}

void useTexture(gfx::Texture* t) {
    if (t != g_batchTex) {
        flush();
        g_batchTex = t;
    }
    if (g_batchCount > 60000) flush();
}

// Room for n more vertices in the batch.
gfx::Vertex* batchAlloc(int n) {
    if (g_batchCount + n > (int)g_batch.size()) g_batch.resize((g_batchCount + n) * 2);
    gfx::Vertex* v = &g_batch[g_batchCount];
    g_batchCount += n;
    return v;
}

// Vertex colour, clamped once per primitive rather than per vertex.
struct VColor {
    float r, g, b, a;
};
VColor vcolor(const ColorF& c) {
    VColor v = {clamp01(c.r), clamp01(c.g), clamp01(c.b), clamp01(c.a)};
    return v;
}

// Screen-only shrink for TVs that crop the picture (b360_config.lua
// B360_SAFE_AREA). Canvases are unaffected; the scale applies when drawing
// to the back buffer, which for Balatro is the final CRT pass.
float g_safeArea = 1.0f;

inline void putVertex(gfx::Vertex& vx, float x, float y, float u, float v, const VColor& c) {
    if (g_safeArea < 1.0f && !g_appliedCanvas) {
        float cx = g_dev->width() * 0.5f, cy = g_dev->height() * 0.5f;
        x = cx + (x - cx) * g_safeArea;
        y = cy + (y - cy) * g_safeArea;
    }
    vx.x = x;
    vx.y = y;
    vx.u = u;
    vx.v = v;
    vx.r = c.r;
    vx.g = c.g;
    vx.b = c.b;
    vx.a = c.a;
}

// Two triangles a-b-c, b-d-c (positions already transformed).
void putQuad(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy, float u0, float v0,
             float u1, float v1, const VColor& c) {
    gfx::Vertex* o = batchAlloc(6);
    putVertex(o[0], ax, ay, u0, v0, c);
    putVertex(o[1], bx, by, u1, v0, c);
    putVertex(o[2], cx, cy, u0, v1, c);
    putVertex(o[3], bx, by, u1, v0, c);
    putVertex(o[4], dx, dy, u1, v1, c);
    putVertex(o[5], cx, cy, u0, v1, c);
}

// Quad from local rectangle with UVs, transformed by m.
void addQuad(const Matrix& m, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
             float v1, const ColorF& c) {
    float ax, ay, bx, by, cx, cy, dx, dy;
    m.apply(x0, y0, ax, ay);
    m.apply(x1, y0, bx, by);
    m.apply(x0, y1, cx, cy);
    m.apply(x1, y1, dx, dy);
    putQuad(ax, ay, bx, by, cx, cy, dx, dy, u0, v0, u1, v1, vcolor(c));
}

ColorF mulColor(const ColorF& a, const ColorF& b) {
    ColorF r;
    r.r = a.r * b.r;
    r.g = a.g * b.g;
    r.b = a.b * b.b;
    r.a = a.a * b.a;
    return r;
}

void applyCanvas() {
    if (g_state.canvas.get() == g_appliedCanvas) return;
    flush();
    g_appliedCanvas = g_state.canvas.get();
    ++g_canvasSwitches;
    g_dev->setRenderTarget(g_appliedCanvas ? g_appliedCanvas->tex : NULL);
    // Scissor rectangles are relative to the target.
    if (g_state.scissor) g_dev->setScissor(true, g_state.sx, g_state.sy, g_state.sw, g_state.sh);
}

void applyState(const GState& prev) {
    if (prev.shader.get() != g_state.shader.get() || prev.blend != g_state.blend ||
        prev.canvas.get() != g_state.canvas.get() || prev.scissor != g_state.scissor ||
        prev.sx != g_state.sx || prev.sy != g_state.sy || prev.sw != g_state.sw || prev.sh != g_state.sh) {
        // Note: flush happened before the state change by the caller.
    }
    applyCanvas();
    g_dev->setScissor(g_state.scissor, g_state.sx, g_state.sy, g_state.sw, g_state.sh);
    g_dev->setColorMask(g_state.mask[0], g_state.mask[1], g_state.mask[2], g_state.mask[3]);
}

void resetState() {
    g_state.color.r = g_state.color.g = g_state.color.b = g_state.color.a = 1;
    g_state.bg.r = g_state.bg.g = g_state.bg.b = 0;
    g_state.bg.a = 1;
    g_state.font = g_defaultFont.get();
    g_state.shader = NULL;
    g_state.canvas = NULL;
    g_state.blend = gfx::BLEND_ALPHA;
    g_state.lineWidth = 1;
    g_state.lineJoin = JOIN_MITER;
    g_state.roughLines = false;
    g_state.pointSize = 1;
    g_state.scissor = false;
    g_state.sx = g_state.sy = g_state.sw = g_state.sh = 0;
    g_state.mask[0] = g_state.mask[1] = g_state.mask[2] = g_state.mask[3] = true;
}

// --------------------------------------------------------------- helpers
gfx::Filter parseFilter(lua_State* L, int idx) {
    const char* s = luaL_checkstring(L, idx);
    if (strcmp(s, "nearest") == 0) return gfx::FILTER_NEAREST;
    if (strcmp(s, "linear") == 0) return gfx::FILTER_LINEAR;
    luaL_error(L, "Invalid filter mode: %s", s);
    return gfx::FILTER_LINEAR;
}

const char* filterName(gfx::Filter f) { return f == gfx::FILTER_NEAREST ? "nearest" : "linear"; }

gfx::Wrap parseWrap(lua_State* L, int idx) {
    const char* s = luaL_checkstring(L, idx);
    if (strcmp(s, "repeat") == 0) return gfx::WRAP_REPEAT;
    if (strcmp(s, "mirroredrepeat") == 0) return gfx::WRAP_MIRROR;
    return gfx::WRAP_CLAMP;
}

const char* wrapName(gfx::Wrap w) {
    return w == gfx::WRAP_REPEAT ? "repeat" : (w == gfx::WRAP_MIRROR ? "mirroredrepeat" : "clamp");
}

// Reads a colour from (r, g, b [, a]) or a table {r, g, b [, a]} at idx.
ColorF readColor(lua_State* L, int idx, float defAlpha) {
    ColorF c;
    if (lua_istable(L, idx)) {
        c.r = (float)luax_numberindex(L, idx, 1, 0);
        c.g = (float)luax_numberindex(L, idx, 2, 0);
        c.b = (float)luax_numberindex(L, idx, 3, 0);
        c.a = (float)luax_numberindex(L, idx, 4, defAlpha);
    } else {
        c.r = (float)luaL_checknumber(L, idx);
        c.g = (float)luaL_checknumber(L, idx + 1);
        c.b = (float)luaL_checknumber(L, idx + 2);
        c.a = (float)luaL_optnumber(L, idx + 3, defAlpha);
    }
    return c;
}

// Reads x, y, r, sx, sy, ox, oy, kx, ky starting at idx.
Matrix readTransform(lua_State* L, int idx) {
    float x = (float)luaL_optnumber(L, idx, 0);
    float y = (float)luaL_optnumber(L, idx + 1, 0);
    float a = (float)luaL_optnumber(L, idx + 2, 0);
    float sx = (float)luaL_optnumber(L, idx + 3, 1);
    float sy = (float)luaL_optnumber(L, idx + 4, sx);
    float ox = (float)luaL_optnumber(L, idx + 5, 0);
    float oy = (float)luaL_optnumber(L, idx + 6, 0);
    float kx = (float)luaL_optnumber(L, idx + 7, 0);
    float ky = (float)luaL_optnumber(L, idx + 8, 0);
    return Matrix::transformation(x, y, a, sx, sy, ox, oy, kx, ky);
}

// Text argument: string/number or LÖVE coloured text table.
void readText(lua_State* L, int idx, std::vector<ColoredString>& out) {
    ColorF white = {1, 1, 1, 1};
    if (lua_istable(L, idx)) {
        int n = (int)lua_objlen(L, idx);
        ColorF cur = white;
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            if (lua_istable(L, -1)) {
                int t = lua_gettop(L);
                cur.r = (float)luax_numberindex(L, t, 1, 1);
                cur.g = (float)luax_numberindex(L, t, 2, 1);
                cur.b = (float)luax_numberindex(L, t, 3, 1);
                cur.a = (float)luax_numberindex(L, t, 4, 1);
            } else if (lua_isstring(L, -1)) {
                ColoredString cs;
                cs.color = cur;
                size_t len;
                const char* s = lua_tolstring(L, -1, &len);
                cs.text.assign(s, len);
                out.push_back(cs);
            }
            lua_pop(L, 1);
        }
        return;
    }
    ColoredString cs;
    cs.color = white;
    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    cs.text.assign(s, len);
    out.push_back(cs);
}

AlignMode parseAlign(const char* s) {
    if (strcmp(s, "center") == 0) return ALIGN_CENTER;
    if (strcmp(s, "right") == 0) return ALIGN_RIGHT;
    if (strcmp(s, "justify") == 0) return ALIGN_JUSTIFY;
    return ALIGN_LEFT;
}

Font* loadFont(lua_State* L, const std::string& path, float size) {
    plat::PathInfo info;
    if (!fs::info(path, info) || info.type != plat::PATH_FILE) {
        luaL_error(L, "Could not open font file %s", path.c_str());
        return NULL;
    }
    // Loaded lazily on first use (see Font).
    return new Font(g_dev, path, size, g_defaultMin, g_defaultMag);
}

void drawGlyphs(const std::vector<GlyphQuad>& quads, const Matrix& m) {
    for (size_t i = 0; i < quads.size(); ++i) {
        const GlyphQuad& q = quads[i];
        useTexture(q.page);
        addQuad(m, q.x0, q.y0, q.x1, q.y1, q.u0, q.v0, q.u1, q.v1, mulColor(q.color, g_state.color));
    }
}

// ------------------------------------------------------------ geometry
typedef std::vector<float> Points;   // x0,y0,x1,y1,...

void fillPolygon(Points& p) {
    size_t n = p.size() / 2;
    if (n < 3) return;
    useTexture(g_white);
    // Transform every point once, then emit the triangle fan.
    for (size_t i = 0; i < n; ++i) {
        float x, y;
        g_transform.apply(p[i * 2], p[i * 2 + 1], x, y);
        p[i * 2] = x;
        p[i * 2 + 1] = y;
    }
    VColor c = vcolor(g_state.color);
    gfx::Vertex* o = batchAlloc((int)(n - 2) * 3);
    for (size_t i = 1; i + 1 < n; ++i) {
        putVertex(*o++, p[0], p[1], 0.5f, 0.5f, c);
        putVertex(*o++, p[i * 2], p[i * 2 + 1], 0.5f, 0.5f, c);
        putVertex(*o++, p[i * 2 + 2], p[i * 2 + 3], 0.5f, 0.5f, c);
    }
}

// Thick polyline with miter joins, generated in local space like LÖVE.
void strokePolyline(Points p, bool closed) {
    size_t n = p.size() / 2;
    if (n < 2) return;
    // Drop consecutive duplicate points.
    Points q;
    for (size_t i = 0; i < n; ++i) {
        if (!q.empty() && q[q.size() - 2] == p[i * 2] && q[q.size() - 1] == p[i * 2 + 1]) continue;
        q.push_back(p[i * 2]);
        q.push_back(p[i * 2 + 1]);
    }
    if (closed && q.size() >= 4 && q[0] == q[q.size() - 2] && q[1] == q[q.size() - 1]) {
        q.pop_back();
        q.pop_back();
    }
    n = q.size() / 2;
    if (n < 2) return;
    float hw = g_state.lineWidth * 0.5f;
    std::vector<float> outer, inner;   // per vertex offsets
    for (size_t i = 0; i < n; ++i) {
        bool hasPrev = closed || i > 0;
        bool hasNext = closed || i + 1 < n;
        size_t ip = (i + n - 1) % n, in = (i + 1) % n;
        float px = q[i * 2], py = q[i * 2 + 1];
        float n1x = 0, n1y = 0, n2x = 0, n2y = 0;
        if (hasPrev) {
            float dx = px - q[ip * 2], dy = py - q[ip * 2 + 1];
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0) {
                n1x = -dy / len;
                n1y = dx / len;
            }
        }
        if (hasNext) {
            float dx = q[in * 2] - px, dy = q[in * 2 + 1] - py;
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0) {
                n2x = -dy / len;
                n2y = dx / len;
            }
        }
        if (!hasPrev) {
            n1x = n2x;
            n1y = n2y;
        }
        if (!hasNext) {
            n2x = n1x;
            n2y = n1y;
        }
        float mx = n1x + n2x, my = n1y + n2y;
        float ml = sqrtf(mx * mx + my * my);
        float ox, oy;
        if (ml < 1e-4f) {
            ox = n1x * hw;
            oy = n1y * hw;
        } else {
            mx /= ml;
            my /= ml;
            float cosHalf = mx * n1x + my * n1y;
            if (cosHalf < 0.25f) cosHalf = 0.25f;   // miter limit
            ox = mx * hw / cosHalf;
            oy = my * hw / cosHalf;
        }
        outer.push_back(px + ox);
        outer.push_back(py + oy);
        inner.push_back(px - ox);
        inner.push_back(py - oy);
    }
    useTexture(g_white);
    VColor col = vcolor(g_state.color);
    size_t segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i) {
        size_t j = (i + 1) % n;
        float ax, ay, bx, by, cx, cy, dx, dy;
        g_transform.apply(outer[i * 2], outer[i * 2 + 1], ax, ay);
        g_transform.apply(inner[i * 2], inner[i * 2 + 1], bx, by);
        g_transform.apply(outer[j * 2], outer[j * 2 + 1], cx, cy);
        g_transform.apply(inner[j * 2], inner[j * 2 + 1], dx, dy);
        putQuad(ax, ay, bx, by, cx, cy, dx, dy, 0.5f, 0.5f, 0.5f, 0.5f, col);
    }
}

void readPoints(lua_State* L, int idx, Points& out) {
    if (lua_istable(L, idx)) {
        int n = (int)lua_objlen(L, idx);
        out.resize(n);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            out[i - 1] = (float)lua_tonumber(L, -1);   // 0 for non-numbers
            lua_pop(L, 1);
        }
    } else {
        for (int i = idx; i <= lua_gettop(L); ++i) out.push_back((float)luaL_checknumber(L, i));
    }
}

bool isFill(lua_State* L, int idx) {
    const char* mode = luaL_checkstring(L, idx);
    return strcmp(mode, "fill") == 0;
}

void ellipsePoints(float x, float y, float rx, float ry, int segs, Points& p) {
    if (segs < 3) {
        float r = rx > ry ? rx : ry;
        segs = (int)(sqrtf(r) * 4);
        if (segs < 8) segs = 8;
        if (segs > 128) segs = 128;
    }
    for (int i = 0; i < segs; ++i) {
        float a = 6.2831853f * i / segs;
        p.push_back(x + rx * cosf(a));
        p.push_back(y + ry * sinf(a));
    }
}

// ----------------------------------------------------------- Lua: objects
TextureObject* checkTexture(lua_State* L, int idx) { return (TextureObject*)luax_checkobject(L, idx, "Texture"); }

int t_getDimensions(lua_State* L) {
    TextureObject* t = checkTexture(L, 1);
    lua_pushnumber(L, t->width());
    lua_pushnumber(L, t->height());
    return 2;
}
int t_getWidth(lua_State* L) {
    lua_pushnumber(L, checkTexture(L, 1)->width());
    return 1;
}
int t_getHeight(lua_State* L) {
    lua_pushnumber(L, checkTexture(L, 1)->height());
    return 1;
}
int t_getPixelDimensions(lua_State* L) {
    TextureObject* t = checkTexture(L, 1);
    lua_pushinteger(L, t->pw);
    lua_pushinteger(L, t->ph);
    return 2;
}
int t_getPixelWidth(lua_State* L) {
    lua_pushinteger(L, checkTexture(L, 1)->pw);
    return 1;
}
int t_getPixelHeight(lua_State* L) {
    lua_pushinteger(L, checkTexture(L, 1)->ph);
    return 1;
}
int t_getDPIScale(lua_State* L) {
    lua_pushnumber(L, checkTexture(L, 1)->dpi);
    return 1;
}
int t_setFilter(lua_State* L) {
    TextureObject* t = checkTexture(L, 1);
    gfx::Filter mi = parseFilter(L, 2);
    gfx::Filter ma = lua_isnoneornil(L, 3) ? mi : parseFilter(L, 3);
    flush();
    g_dev->setTextureParams(t->tex, mi, ma, t->tex->wrapS, t->tex->wrapT);
    return 0;
}
int t_getFilter(lua_State* L) {
    TextureObject* t = checkTexture(L, 1);
    lua_pushstring(L, filterName(t->tex->minFilter));
    lua_pushstring(L, filterName(t->tex->magFilter));
    lua_pushnumber(L, 1);
    return 3;
}
int t_setWrap(lua_State* L) {
    TextureObject* t = checkTexture(L, 1);
    gfx::Wrap ws = parseWrap(L, 2);
    gfx::Wrap wt = lua_isnoneornil(L, 3) ? ws : parseWrap(L, 3);
    flush();
    g_dev->setTextureParams(t->tex, t->tex->minFilter, t->tex->magFilter, ws, wt);
    return 0;
}
int t_getWrap(lua_State* L) {
    TextureObject* t = checkTexture(L, 1);
    lua_pushstring(L, wrapName(t->tex->wrapS));
    lua_pushstring(L, wrapName(t->tex->wrapT));
    return 2;
}
int t_noop(lua_State* L) {
    (void)L;
    return 0;
}
int t_getFormat(lua_State* L) {
    lua_pushstring(L, "rgba8");
    return 1;
}
int t_getMipmapCount(lua_State* L) {
    lua_pushinteger(L, 1);
    return 1;
}
int t_getTextureType(lua_State* L) {
    lua_pushstring(L, "2d");
    return 1;
}
int t_isReadable(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}
int t_getMSAA(lua_State* L) {
    lua_pushinteger(L, 0);
    return 1;
}

int c_renderTo(lua_State* L);

const luaL_Reg kTextureMethods[] = {
    {"getDimensions", t_getDimensions}, {"getWidth", t_getWidth}, {"getHeight", t_getHeight},
    {"getPixelDimensions", t_getPixelDimensions}, {"getPixelWidth", t_getPixelWidth},
    {"getPixelHeight", t_getPixelHeight}, {"getDPIScale", t_getDPIScale}, {"setFilter", t_setFilter},
    {"getFilter", t_getFilter}, {"setWrap", t_setWrap}, {"getWrap", t_getWrap},
    {"setMipmapFilter", t_noop}, {"getFormat", t_getFormat}, {"getMipmapCount", t_getMipmapCount},
    {"getTextureType", t_getTextureType}, {"isReadable", t_isReadable}, {NULL, NULL}};

const luaL_Reg kCanvasMethods[] = {{"renderTo", c_renderTo}, {"getMSAA", t_getMSAA}, {NULL, NULL}};

Quad* checkQuad(lua_State* L, int idx) { return (Quad*)luax_checkobject(L, idx, "Quad"); }

int q_setViewport(lua_State* L) {
    Quad* q = checkQuad(L, 1);
    q->x = luaL_checknumber(L, 2);
    q->y = luaL_checknumber(L, 3);
    q->w = luaL_checknumber(L, 4);
    q->h = luaL_checknumber(L, 5);
    if (lua_isnumber(L, 6)) {
        q->sw = lua_tonumber(L, 6);
        q->sh = luaL_checknumber(L, 7);
    }
    return 0;
}
int q_getViewport(lua_State* L) {
    Quad* q = checkQuad(L, 1);
    lua_pushnumber(L, q->x);
    lua_pushnumber(L, q->y);
    lua_pushnumber(L, q->w);
    lua_pushnumber(L, q->h);
    return 4;
}
int q_getTextureDimensions(lua_State* L) {
    Quad* q = checkQuad(L, 1);
    lua_pushnumber(L, q->sw);
    lua_pushnumber(L, q->sh);
    return 2;
}

const luaL_Reg kQuadMethods[] = {
    {"setViewport", q_setViewport}, {"getViewport", q_getViewport},
    {"getTextureDimensions", q_getTextureDimensions}, {NULL, NULL}};

ShaderObject* checkShader(lua_State* L, int idx) { return (ShaderObject*)luax_checkobject(L, idx, "Shader"); }

int sh_send(lua_State* L) {
    ShaderObject* s = checkShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    int ui = s->find(name);
    if (ui < 0) return 0;   // LÖVE errors here; we are lenient.
    const gfx::UniformInfo* u = &s->sh->uniforms[ui];
    if (u->type == gfx::UNIFORM_SAMPLER) {
        TextureObject* t = checkTexture(L, 3);
        if (g_state.shader.get() == s) flush();
        s->sh->setUniformTexture(ui, t->tex);
        return 0;
    }
    int comps = u->type == gfx::UNIFORM_MAT ? u->components * u->components : u->components;
    FloatBuf vals;
    int nargs = lua_gettop(L) - 2;
    int count = nargs < u->arraySize ? nargs : u->arraySize;
    if (count < 1) count = 1;
    for (int i = 0; i < count; ++i) {
        int idx = 3 + i;
        int type = lua_type(L, idx);
        if (type == LUA_TTABLE) {
            int n = (int)lua_objlen(L, idx);
            bool nested = n > 0 && (lua_rawgeti(L, idx, 1), lua_istable(L, -1));
            if (n > 0) lua_pop(L, 1);
            if (nested) {
                // matrix as table of rows; LÖVE uses column-major by default
                for (int r = 1; r <= n; ++r) {
                    lua_rawgeti(L, idx, r);
                    int t = lua_gettop(L);
                    for (int c = 1; c <= u->components; ++c) vals.push((float)luax_numberindex(L, t, c, 0));
                    lua_pop(L, 1);
                }
            } else {
                for (int k = 1; k <= comps; ++k) {
                    lua_rawgeti(L, idx, k);
                    if (lua_isboolean(L, -1)) vals.push(lua_toboolean(L, -1) ? 1.0f : 0.0f);
                    else vals.push((float)lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
            }
        } else if (type == LUA_TBOOLEAN) {
            vals.push(lua_toboolean(L, idx) ? 1.0f : 0.0f);
            for (int k = 1; k < comps; ++k) vals.push(0);
        } else {
            vals.push((float)luaL_checknumber(L, idx));
            for (int k = 1; k < comps; ++k) vals.push(0);
        }
    }
    if (s->values.size() != s->sh->uniforms.size()) s->values.resize(s->sh->uniforms.size());
    std::vector<float>& cur = s->values[ui];
    if ((int)cur.size() == vals.n && memcmp(&cur[0], vals.data(), vals.n * sizeof(float)) == 0) return 0;
    if (g_state.shader.get() == s) flush();
    cur.assign(vals.data(), vals.data() + vals.n);
    s->sh->setUniform(ui, vals.data(), vals.n);
    return 0;
}

int sh_hasUniform(lua_State* L) {
    ShaderObject* s = checkShader(L, 1);
    lua_pushboolean(L, s->find(luaL_checkstring(L, 2)) >= 0);
    return 1;
}

int sh_getWarnings(lua_State* L) {
    lua_pushstring(L, "");
    return 1;
}

const luaL_Reg kShaderMethods[] = {
    {"send", sh_send}, {"sendColor", sh_send}, {"hasUniform", sh_hasUniform},
    {"getWarnings", sh_getWarnings}, {NULL, NULL}};

Font* checkFont(lua_State* L, int idx) { return (Font*)luax_checkobject(L, idx, "Font"); }

int f_getWidth(lua_State* L) {
    Font* f = checkFont(L, 1);
    lua_pushnumber(L, f->getWidth(luax_checkstring(L, 2)));
    return 1;
}
int f_getHeight(lua_State* L) {
    lua_pushnumber(L, checkFont(L, 1)->getHeight());
    return 1;
}
int f_getAscent(lua_State* L) {
    lua_pushnumber(L, checkFont(L, 1)->getAscent());
    return 1;
}
int f_getDescent(lua_State* L) {
    lua_pushnumber(L, checkFont(L, 1)->getDescent());
    return 1;
}
int f_getBaseline(lua_State* L) {
    lua_pushnumber(L, checkFont(L, 1)->getBaseline());
    return 1;
}
int f_getLineHeight(lua_State* L) {
    lua_pushnumber(L, checkFont(L, 1)->getLineHeight());
    return 1;
}
int f_setLineHeight(lua_State* L) {
    checkFont(L, 1)->setLineHeight((float)luaL_checknumber(L, 2));
    return 0;
}
int f_setFilter(lua_State* L) {
    Font* f = checkFont(L, 1);
    gfx::Filter mi = parseFilter(L, 2);
    gfx::Filter ma = lua_isnoneornil(L, 3) ? mi : parseFilter(L, 3);
    flush();
    f->setFilter(mi, ma);
    return 0;
}
int f_getFilter(lua_State* L) {
    Font* f = checkFont(L, 1);
    lua_pushstring(L, filterName(f->minFilter()));
    lua_pushstring(L, filterName(f->magFilter()));
    lua_pushnumber(L, 1);
    return 3;
}
int f_getWrap(lua_State* L) {
    Font* f = checkFont(L, 1);
    std::vector<ColoredString> text;
    readText(L, 2, text);
    float limit = (float)luaL_checknumber(L, 3);
    std::vector<std::vector<ColoredString> > lines;
    std::vector<float> widths;
    f->wrap(text, limit, lines, widths);
    float maxW = 0;
    lua_createtable(L, (int)lines.size(), 0);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string s;
        for (size_t k = 0; k < lines[i].size(); ++k) s += lines[i][k].text;
        lua_pushlstring(L, s.data(), s.size());
        lua_rawseti(L, -2, (int)i + 1);
        if (widths[i] > maxW) maxW = widths[i];
    }
    lua_pushnumber(L, maxW);
    lua_insert(L, -2);
    return 2;
}
int f_hasGlyphs(lua_State* L) {
    Font* f = checkFont(L, 1);
    for (int i = 2; i <= lua_gettop(L); ++i) {
        if (lua_isnumber(L, i)) {
            if (!f->hasGlyph((u32)lua_tonumber(L, i))) {
                lua_pushboolean(L, 0);
                return 1;
            }
            continue;
        }
        std::string s = luax_checkstring(L, i);
        size_t k = 0;
        while (k < s.size()) {
            if (!f->hasGlyph(Font::decodeUTF8(s, k))) {
                lua_pushboolean(L, 0);
                return 1;
            }
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}
int f_getDPIScale(lua_State* L) {
    lua_pushnumber(L, 1);
    return 1;
}
int f_getKerning(lua_State* L) {
    Font* f = checkFont(L, 1);
    u32 a, b;
    if (lua_isnumber(L, 2)) {
        a = (u32)lua_tonumber(L, 2);
        b = (u32)luaL_checknumber(L, 3);
    } else {
        std::string sa = luax_checkstring(L, 2), sb = luax_checkstring(L, 3);
        size_t i = 0, j = 0;
        a = sa.empty() ? 0 : Font::decodeUTF8(sa, i);
        b = sb.empty() ? 0 : Font::decodeUTF8(sb, j);
    }
    lua_pushnumber(L, f->getKerning(a, b));
    return 1;
}

const luaL_Reg kFontMethods[] = {
    {"getWidth", f_getWidth}, {"getHeight", f_getHeight}, {"getAscent", f_getAscent},
    {"getDescent", f_getDescent}, {"getBaseline", f_getBaseline}, {"getLineHeight", f_getLineHeight},
    {"setLineHeight", f_setLineHeight}, {"setFilter", f_setFilter}, {"getFilter", f_getFilter},
    {"getWrap", f_getWrap}, {"hasGlyphs", f_hasGlyphs}, {"getDPIScale", f_getDPIScale},
    {"getKerning", f_getKerning}, {"setFallbacks", t_noop}, {NULL, NULL}};

TextObject* checkText(lua_State* L, int idx) { return (TextObject*)luax_checkobject(L, idx, "Text"); }

int tx_set(lua_State* L) {
    TextObject* t = checkText(L, 1);
    t->entries.clear();
    t->dirty = true;
    if (lua_isnoneornil(L, 2)) return 0;
    TextObject::Entry e;
    readText(L, 2, e.text);
    e.limit = -1;
    e.align = ALIGN_LEFT;
    t->entries.push_back(e);
    return 0;
}
int tx_setf(lua_State* L) {
    TextObject* t = checkText(L, 1);
    t->entries.clear();
    t->dirty = true;
    TextObject::Entry e;
    readText(L, 2, e.text);
    e.limit = (float)luaL_checknumber(L, 3);
    e.align = parseAlign(luaL_optstring(L, 4, "left"));
    t->entries.push_back(e);
    return 0;
}
int tx_add(lua_State* L) {
    TextObject* t = checkText(L, 1);
    TextObject::Entry e;
    readText(L, 2, e.text);
    e.limit = -1;
    e.align = ALIGN_LEFT;
    e.m = readTransform(L, 3);
    t->entries.push_back(e);
    t->dirty = true;
    lua_pushinteger(L, (int)t->entries.size());
    return 1;
}
int tx_addf(lua_State* L) {
    TextObject* t = checkText(L, 1);
    TextObject::Entry e;
    readText(L, 2, e.text);
    e.limit = (float)luaL_checknumber(L, 3);
    e.align = parseAlign(luaL_checkstring(L, 4));
    e.m = readTransform(L, 5);
    t->entries.push_back(e);
    t->dirty = true;
    lua_pushinteger(L, (int)t->entries.size());
    return 1;
}
int tx_clear(lua_State* L) {
    TextObject* t = checkText(L, 1);
    t->entries.clear();
    t->dirty = true;
    return 0;
}
int tx_getWidth(lua_State* L) {
    TextObject* t = checkText(L, 1);
    t->rebuild();
    lua_pushnumber(L, t->width);
    return 1;
}
int tx_getHeight(lua_State* L) {
    TextObject* t = checkText(L, 1);
    t->rebuild();
    lua_pushnumber(L, t->height);
    return 1;
}
int tx_getDimensions(lua_State* L) {
    TextObject* t = checkText(L, 1);
    t->rebuild();
    lua_pushnumber(L, t->width);
    lua_pushnumber(L, t->height);
    return 2;
}
int tx_getFont(lua_State* L) {
    luax_pushobject(L, "Font", checkText(L, 1)->font.get());
    return 1;
}
int tx_setFont(lua_State* L) {
    TextObject* t = checkText(L, 1);
    t->font = checkFont(L, 2);
    t->dirty = true;
    return 0;
}

const luaL_Reg kTextMethods[] = {
    {"set", tx_set}, {"setf", tx_setf}, {"add", tx_add}, {"addf", tx_addf}, {"clear", tx_clear},
    {"getWidth", tx_getWidth}, {"getHeight", tx_getHeight}, {"getDimensions", tx_getDimensions},
    {"getFont", tx_getFont}, {"setFont", tx_setFont}, {NULL, NULL}};

// ------------------------------------------------------- Lua: constructors
int w_newImage(lua_State* L) {
    std::string path = luax_checkstring(L, 1);
    std::vector<u8> data;
    if (!fs::read(path, data)) return luaL_error(L, "Could not open file %s. Does not exist.", path.c_str());
    int w = 0, h = 0, comp = 0;
    u8* pixels = stbi_load_from_memory(&data[0], (int)data.size(), &w, &h, &comp, 4);
    if (!pixels) return luaL_error(L, "Could not decode image %s: %s", path.c_str(), stbi_failure_reason());
    bool mipmaps = false;
    float dpi = 1;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "mipmaps");
        mipmaps = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        dpi = (float)luax_numberfield(L, 2, "dpiscale", 1);
        if (dpi <= 0) dpi = 1;
    }
    Image* img = new Image;
    img->pw = w;
    img->ph = h;
    img->dpi = dpi;
    img->mipmaps = mipmaps;
    img->tex = g_dev->createTexture(w, h, pixels, mipmaps);
    stbi_image_free(pixels);
    g_dev->setTextureParams(img->tex, g_defaultMin, g_defaultMag, gfx::WRAP_CLAMP, gfx::WRAP_CLAMP);
    luax_pushobject(L, "Image", img);
    img->release();
    return 1;
}

int w_newCanvas(lua_State* L) {
    int w = (int)luaL_optnumber(L, 1, g_dev->width());
    int h = (int)luaL_optnumber(L, 2, g_dev->height());
    float dpi = 1;
    if (lua_istable(L, 3)) dpi = (float)luax_numberfield(L, 3, "dpiscale", 1);
    if (dpi <= 0) dpi = 1;
    Canvas* c = new Canvas;
    c->pw = (int)(w * dpi + 0.5f);
    c->ph = (int)(h * dpi + 0.5f);
    if (c->pw < 1) c->pw = 1;
    if (c->ph < 1) c->ph = 1;
    c->dpi = dpi;
    c->tex = g_dev->createRenderTarget(c->pw, c->ph);
    if (!c->tex) {
        c->release();
        return luaL_error(L, "Could not create canvas %dx%d", w, h);
    }
    g_dev->setTextureParams(c->tex, g_defaultMin, g_defaultMag, gfx::WRAP_CLAMP, gfx::WRAP_CLAMP);
    // createRenderTarget may have changed the bound target.
    g_appliedCanvas = (Canvas*)1;
    applyCanvas();
    luax_pushobject(L, "Canvas", c);
    c->release();
    return 1;
}

int w_newQuad(lua_State* L) {
    Quad* q = new Quad;
    q->x = luaL_checknumber(L, 1);
    q->y = luaL_checknumber(L, 2);
    q->w = luaL_checknumber(L, 3);
    q->h = luaL_checknumber(L, 4);
    if (luax_toobject(L, 5, "Texture")) {
        TextureObject* t = checkTexture(L, 5);
        q->sw = t->width();
        q->sh = t->height();
    } else {
        q->sw = luaL_checknumber(L, 5);
        q->sh = luaL_checknumber(L, 6);
    }
    luax_pushobject(L, "Quad", q);
    q->release();
    return 1;
}

int w_newShader(lua_State* L) {
    std::string src;
    for (int i = 1; i <= 2 && !lua_isnoneornil(L, i); ++i) {
        std::string arg = luax_checkstring(L, i);
        std::string fileData;
        bool looksLikeCode = arg.find('\n') != std::string::npos || arg.find(';') != std::string::npos;
        if (!looksLikeCode && fs::read(arg, fileData)) src += fileData;
        else src += arg;
        src += "\n";
    }
    std::string err;
    gfx::Shader* sh = g_dev->createShader(src, err);
    if (!sh) return luaL_error(L, "Cannot compile shader:\n%s", err.c_str());
    ShaderObject* s = new ShaderObject;
    s->sh = sh;
    luax_pushobject(L, "Shader", s);
    s->release();
    return 1;
}

int w_newFont(lua_State* L) {
    Font* f;
    if (lua_type(L, 1) == LUA_TSTRING) {
        std::string path = luax_checkstring(L, 1);
        float size = (float)luaL_optnumber(L, 2, 12);
        f = loadFont(L, path, size);
    } else {
        // newFont([size]): no built-in Vera font, use the game's pixel font.
        float size = (float)luaL_optnumber(L, 1, 12);
        f = loadFont(L, "resources/fonts/m6x11plus.ttf", size);
    }
    luax_pushobject(L, "Font", f);
    f->release();
    return 1;
}

int w_setFont(lua_State* L) {
    g_state.font = checkFont(L, 1);
    return 0;
}

int w_getFont(lua_State* L) {
    if (!g_state.font.get()) {
        lua_getglobal(L, "love");
        lua_getfield(L, -1, "graphics");
        lua_getfield(L, -1, "newFont");
        lua_pushnumber(L, 12);
        lua_call(L, 1, 1);
        g_state.font = checkFont(L, -1);
        g_defaultFont = g_state.font.get();
        return 1;
    }
    luax_pushobject(L, "Font", g_state.font.get());
    return 1;
}

int w_setNewFont(lua_State* L) {
    w_newFont(L);
    g_state.font = checkFont(L, -1);
    return 1;
}

int w_newText(lua_State* L) {
    Font* f = checkFont(L, 1);
    TextObject* t = new TextObject;
    t->font = f;
    if (!lua_isnoneornil(L, 2)) {
        TextObject::Entry e;
        readText(L, 2, e.text);
        e.limit = -1;
        e.align = ALIGN_LEFT;
        t->entries.push_back(e);
    }
    luax_pushobject(L, "Text", t);
    t->release();
    return 1;
}

int w_newVideo(lua_State* L) { return luaL_error(L, "Video playback is not supported on this platform"); }

// ------------------------------------------------------------ Lua: state
int w_setColor(lua_State* L) {
    g_state.color = readColor(L, 1, 1.0f);
    return 0;
}
int w_getColor(lua_State* L) {
    lua_pushnumber(L, g_state.color.r);
    lua_pushnumber(L, g_state.color.g);
    lua_pushnumber(L, g_state.color.b);
    lua_pushnumber(L, g_state.color.a);
    return 4;
}
int w_setBackgroundColor(lua_State* L) {
    g_state.bg = readColor(L, 1, 1.0f);
    return 0;
}
int w_getBackgroundColor(lua_State* L) {
    lua_pushnumber(L, g_state.bg.r);
    lua_pushnumber(L, g_state.bg.g);
    lua_pushnumber(L, g_state.bg.b);
    lua_pushnumber(L, g_state.bg.a);
    return 4;
}

int w_setShader(lua_State* L) {
    ShaderObject* s = lua_isnoneornil(L, 1) ? NULL : checkShader(L, 1);
    if (s != g_state.shader.get()) {
        flush();
        g_state.shader = s;
    }
    return 0;
}
int w_getShader(lua_State* L) {
    luax_pushobject(L, "Shader", g_state.shader.get());
    return 1;
}

int w_setCanvas(lua_State* L) {
    Canvas* c = NULL;
    if (lua_istable(L, 1)) {
        lua_rawgeti(L, 1, 1);
        if (lua_istable(L, -1)) lua_rawgeti(L, -1, 1);   // {{canvas, mipmap=...}}
        if (!lua_isnil(L, -1)) c = (Canvas*)luax_checkobject(L, -1, "Canvas");
        lua_settop(L, 1);
    } else if (!lua_isnoneornil(L, 1)) {
        c = (Canvas*)luax_checkobject(L, 1, "Canvas");
    }
    if (c != g_state.canvas.get()) {
        flush();
        g_state.canvas = c;
        applyCanvas();
    }
    return 0;
}
int w_getCanvas(lua_State* L) {
    luax_pushobject(L, "Canvas", g_state.canvas.get());
    return 1;
}

int c_renderTo(lua_State* L) {
    Canvas* c = (Canvas*)luax_checkobject(L, 1, "Canvas");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    Ref<Canvas> prev = g_state.canvas;
    flush();
    g_state.canvas = c;
    applyCanvas();
    lua_pushvalue(L, 2);
    int status = lua_pcall(L, 0, 0, 0);
    flush();
    g_state.canvas = prev;
    applyCanvas();
    if (status != 0) return lua_error(L);
    return 0;
}

int w_clear(lua_State* L) {
    ColorF c = {0, 0, 0, 0};
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        if (lua_istable(L, 1)) c = readColor(L, 1, 1.0f);
        else if (lua_isnumber(L, 1)) c = readColor(L, 1, 1.0f);
    }
    flush();
    applyCanvas();
    g_dev->clear(c.r, c.g, c.b, c.a);
    return 0;
}

int w_present(lua_State* L) {
    (void)L;
    flush();
    std::string shot;
    if (plat::screenshotRequested(shot)) {
        std::vector<u8> rgba;
        if (g_dev->readBackbuffer(rgba)) {
            if (stbi_write_png(shot.c_str(), g_dev->width(), g_dev->height(), 4, &rgba[0], g_dev->width() * 4))
                plat::log("[graphics] screenshot written to %s", shot.c_str());
            else
                plat::log("[graphics] failed to write %s", shot.c_str());
        }
    }
    g_pacer.beforePresent(g_dev);
    g_dev->present();
    g_pacer.afterPresent(g_dev);
    profilerFrame();
    g_drawCallsLast = g_drawCalls;
    g_drawCalls = 0;
    g_canvasSwitchesLast = g_canvasSwitches;
    g_canvasSwitches = 0;
    g_verticesLast = g_vertices;
    g_vertices = 0;
    return 0;
}

int w_setBlendMode(lua_State* L) {
    const char* mode = luaL_checkstring(L, 1);
    const char* alpha = luaL_optstring(L, 2, "alphamultiply");
    bool premul = strcmp(alpha, "premultiplied") == 0;
    gfx::BlendMode b = gfx::BLEND_ALPHA;
    if (strcmp(mode, "alpha") == 0) b = premul ? gfx::BLEND_ALPHA_PREMUL : gfx::BLEND_ALPHA;
    else if (strcmp(mode, "add") == 0) b = gfx::BLEND_ADD;
    else if (strcmp(mode, "subtract") == 0) b = gfx::BLEND_SUBTRACT;
    else if (strcmp(mode, "multiply") == 0) b = gfx::BLEND_MULTIPLY;
    else if (strcmp(mode, "replace") == 0) b = gfx::BLEND_REPLACE;
    else if (strcmp(mode, "screen") == 0) b = gfx::BLEND_SCREEN;
    if (b != g_state.blend) {
        flush();
        g_state.blend = b;
    }
    return 0;
}
int w_getBlendMode(lua_State* L) {
    const char* names[] = {"alpha", "alpha", "add", "subtract", "multiply", "replace", "screen"};
    lua_pushstring(L, names[g_state.blend]);
    lua_pushstring(L, g_state.blend == gfx::BLEND_ALPHA_PREMUL ? "premultiplied" : "alphamultiply");
    return 2;
}

int w_setScissor(lua_State* L) {
    flush();
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        g_state.scissor = false;
    } else {
        // Scissor is in screen pixels (not affected by the transform).
        g_state.scissor = true;
        g_state.sx = (int)luaL_checknumber(L, 1);
        g_state.sy = (int)luaL_checknumber(L, 2);
        g_state.sw = (int)luaL_checknumber(L, 3);
        g_state.sh = (int)luaL_checknumber(L, 4);
        if (g_state.sw < 0) g_state.sw = 0;
        if (g_state.sh < 0) g_state.sh = 0;
    }
    g_dev->setScissor(g_state.scissor, g_state.sx, g_state.sy, g_state.sw, g_state.sh);
    return 0;
}
int w_getScissor(lua_State* L) {
    if (!g_state.scissor) return 0;
    lua_pushinteger(L, g_state.sx);
    lua_pushinteger(L, g_state.sy);
    lua_pushinteger(L, g_state.sw);
    lua_pushinteger(L, g_state.sh);
    return 4;
}
int w_intersectScissor(lua_State* L) {
    int x = (int)luaL_checknumber(L, 1), y = (int)luaL_checknumber(L, 2);
    int w = (int)luaL_checknumber(L, 3), h = (int)luaL_checknumber(L, 4);
    if (g_state.scissor) {
        int x2 = x + w, y2 = y + h;
        int sx2 = g_state.sx + g_state.sw, sy2 = g_state.sy + g_state.sh;
        if (x < g_state.sx) x = g_state.sx;
        if (y < g_state.sy) y = g_state.sy;
        if (x2 > sx2) x2 = sx2;
        if (y2 > sy2) y2 = sy2;
        w = x2 - x;
        h = y2 - y;
    }
    lua_settop(L, 0);
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    lua_pushinteger(L, w < 0 ? 0 : w);
    lua_pushinteger(L, h < 0 ? 0 : h);
    return w_setScissor(L);
}

int w_setColorMask(lua_State* L) {
    flush();
    if (lua_gettop(L) == 0) {
        g_state.mask[0] = g_state.mask[1] = g_state.mask[2] = g_state.mask[3] = true;
    } else {
        for (int i = 0; i < 4; ++i) g_state.mask[i] = lua_toboolean(L, i + 1) != 0;
    }
    g_dev->setColorMask(g_state.mask[0], g_state.mask[1], g_state.mask[2], g_state.mask[3]);
    return 0;
}

int w_setLineWidth(lua_State* L) {
    g_state.lineWidth = (float)luaL_checknumber(L, 1);
    return 0;
}
int w_getLineWidth(lua_State* L) {
    lua_pushnumber(L, g_state.lineWidth);
    return 1;
}
int w_setLineStyle(lua_State* L) {
    g_state.roughLines = strcmp(luaL_checkstring(L, 1), "rough") == 0;
    return 0;
}
int w_getLineStyle(lua_State* L) {
    lua_pushstring(L, g_state.roughLines ? "rough" : "smooth");
    return 1;
}
int w_setLineJoin(lua_State* L) {
    const char* j = luaL_checkstring(L, 1);
    g_state.lineJoin = strcmp(j, "bevel") == 0 ? JOIN_BEVEL : (strcmp(j, "none") == 0 ? JOIN_NONE : JOIN_MITER);
    return 0;
}
int w_getLineJoin(lua_State* L) {
    const char* names[] = {"miter", "bevel", "none"};
    lua_pushstring(L, names[g_state.lineJoin]);
    return 1;
}
int w_setPointSize(lua_State* L) {
    g_state.pointSize = (float)luaL_checknumber(L, 1);
    return 0;
}
int w_getPointSize(lua_State* L) {
    lua_pushnumber(L, g_state.pointSize);
    return 1;
}

int w_setDefaultFilter(lua_State* L) {
    g_defaultMin = parseFilter(L, 1);
    g_defaultMag = lua_isnoneornil(L, 2) ? g_defaultMin : parseFilter(L, 2);
    return 0;
}
int w_getDefaultFilter(lua_State* L) {
    lua_pushstring(L, filterName(g_defaultMin));
    lua_pushstring(L, filterName(g_defaultMag));
    lua_pushnumber(L, 1);
    return 3;
}

// -------------------------------------------------------- Lua: transforms
int w_push(lua_State* L) {
    if (g_stack.size() >= 64) return luaL_error(L, "Maximum stack depth reached (more pushes than pops?)");
    StackEntry e;
    e.m = g_transform;
    e.hasState = lua_isstring(L, 1) && strcmp(lua_tostring(L, 1), "all") == 0;
    if (e.hasState) e.state = g_state;
    g_stack.push_back(e);
    return 0;
}
int w_pop(lua_State* L) {
    if (g_stack.empty()) return luaL_error(L, "Minimum stack depth reached (more pops than pushes?)");
    StackEntry& e = g_stack.back();
    g_transform = e.m;
    if (e.hasState) {
        flush();
        GState prev = g_state;
        g_state = e.state;
        applyState(prev);
    }
    g_stack.pop_back();
    return 0;
}
int w_origin(lua_State* L) {
    (void)L;
    g_transform = Matrix();
    return 0;
}
int w_translate(lua_State* L) {
    Matrix m;
    m.e = (float)luaL_checknumber(L, 1);
    m.f = (float)luaL_checknumber(L, 2);
    g_transform = g_transform * m;
    return 0;
}
int w_scale(lua_State* L) {
    Matrix m;
    m.a = (float)luaL_checknumber(L, 1);
    m.d = (float)luaL_optnumber(L, 2, m.a);
    g_transform = g_transform * m;
    return 0;
}
int w_rotate(lua_State* L) {
    float a = (float)luaL_checknumber(L, 1);
    Matrix m;
    m.a = cosf(a);
    m.b = sinf(a);
    m.c = -m.b;
    m.d = m.a;
    g_transform = g_transform * m;
    return 0;
}
int w_shear(lua_State* L) {
    Matrix m;
    m.c = (float)luaL_checknumber(L, 1);
    m.b = (float)luaL_optnumber(L, 2, 0);
    g_transform = g_transform * m;
    return 0;
}
int w_transformPoint(lua_State* L) {
    float x, y;
    g_transform.apply((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), x, y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}
int w_inverseTransformPoint(lua_State* L) {
    Matrix inv;
    g_transform.invert(inv);
    float x, y;
    inv.apply((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), x, y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}
int w_getStackDepth(lua_State* L) {
    lua_pushinteger(L, (int)g_stack.size());
    return 1;
}

int w_reset(lua_State* L) {
    (void)L;
    flush();
    GState prev = g_state;
    resetState();
    g_transform = Matrix();
    g_stack.clear();
    applyState(prev);
    return 0;
}

// ------------------------------------------------------------- Lua: draw
int w_draw(lua_State* L) {
    const char* type = luax_objecttype(L, 1);
    if (!type) return luaL_error(L, "bad argument #1 to 'draw' (Drawable expected)");
    applyCanvas();
    if (luax_istype(type, "Texture")) {
        TextureObject* t = checkTexture(L, 1);
        if (t == (TextureObject*)g_state.canvas.get())
            return luaL_error(L, "Cannot render a Canvas to itself!");
        Quad* q = (Quad*)luax_toobject(L, 2, "Quad");
        Matrix m = g_transform * readTransform(L, q ? 3 : 2);
        useTexture(t->tex);
        if (q) {
            float u0 = (float)(q->x / q->sw), v0 = (float)(q->y / q->sh);
            float u1 = (float)((q->x + q->w) / q->sw), v1 = (float)((q->y + q->h) / q->sh);
            addQuad(m, 0, 0, (float)q->w, (float)q->h, u0, v0, u1, v1, g_state.color);
        } else {
            addQuad(m, 0, 0, t->width(), t->height(), 0, 0, 1, 1, g_state.color);
        }
        return 0;
    }
    if (strcmp(type, "Text") == 0) {
        TextObject* t = checkText(L, 1);
        t->rebuild();
        drawGlyphs(t->quads, g_transform * readTransform(L, 2));
        return 0;
    }
    return luaL_error(L, "Drawing a %s is not supported", type);
}

int printImpl(lua_State* L, bool formatted) {
    std::vector<ColoredString> text;
    readText(L, 1, text);
    int idx = 2;
    Font* font = (Font*)luax_toobject(L, 2, "Font");
    if (font) {
        idx = 3;
    } else {
        if (!g_state.font.get()) {
            w_getFont(L);
            lua_pop(L, 1);
        }
        font = g_state.font.get();
    }
    applyCanvas();
    std::vector<GlyphQuad> quads;
    Matrix m;
    if (formatted) {
        if (lua_gettop(L) < idx + 3) lua_settop(L, idx + 3);
        float x = (float)luaL_optnumber(L, idx, 0);
        float y = (float)luaL_optnumber(L, idx + 1, 0);
        float limit = (float)luaL_checknumber(L, idx + 2);
        AlignMode align = parseAlign(luaL_optstring(L, idx + 3, "left"));
        lua_pushnumber(L, x);
        lua_replace(L, idx + 2);
        lua_pushnumber(L, y);
        lua_replace(L, idx + 3);
        font->layout(text, limit, align, quads, NULL);
        m = readTransform(L, idx + 2);
    } else {
        font->layout(text, -1, ALIGN_LEFT, quads, NULL);
        m = readTransform(L, idx);
    }
    drawGlyphs(quads, g_transform * m);
    return 0;
}
int w_print(lua_State* L) { return printImpl(L, false); }
int w_printf(lua_State* L) { return printImpl(L, true); }

int w_rectangle(lua_State* L) {
    bool fill = isFill(L, 1);
    float x = (float)luaL_checknumber(L, 2), y = (float)luaL_checknumber(L, 3);
    float w = (float)luaL_checknumber(L, 4), h = (float)luaL_checknumber(L, 5);
    applyCanvas();
    Points p;
    if (lua_isnumber(L, 6)) {
        float rx = (float)lua_tonumber(L, 6);
        float ry = (float)luaL_optnumber(L, 7, rx);
        if (rx > w / 2) rx = w / 2;
        if (ry > h / 2) ry = h / 2;
        int segs = (int)luaL_optnumber(L, 8, 0);
        if (segs < 1) {
            float r = rx > ry ? rx : ry;
            segs = (int)(sqrtf(r) * 2);
            if (segs < 3) segs = 3;
            if (segs > 32) segs = 32;
        }
        const float hp = 1.5707963f;
        float cx[4] = {x + w - rx, x + rx, x + rx, x + w - rx};
        float cy[4] = {y + h - ry, y + h - ry, y + ry, y + ry};
        for (int c = 0; c < 4; ++c) {
            for (int i = 0; i <= segs; ++i) {
                float a = hp * c + hp * i / segs;
                p.push_back(cx[c] + rx * cosf(a));
                p.push_back(cy[c] + ry * sinf(a));
            }
        }
    } else {
        p.push_back(x);
        p.push_back(y);
        p.push_back(x + w);
        p.push_back(y);
        p.push_back(x + w);
        p.push_back(y + h);
        p.push_back(x);
        p.push_back(y + h);
    }
    if (fill) fillPolygon(p);
    else strokePolyline(p, true);
    return 0;
}

int w_polygon(lua_State* L) {
    bool fill = isFill(L, 1);
    Points p;
    readPoints(L, 2, p);
    applyCanvas();
    if (fill) fillPolygon(p);
    else strokePolyline(p, true);
    return 0;
}

int w_line(lua_State* L) {
    Points p;
    readPoints(L, 1, p);
    applyCanvas();
    strokePolyline(p, false);
    return 0;
}

int w_circle(lua_State* L) {
    bool fill = isFill(L, 1);
    float x = (float)luaL_checknumber(L, 2), y = (float)luaL_checknumber(L, 3);
    float r = (float)luaL_checknumber(L, 4);
    Points p;
    ellipsePoints(x, y, r, r, (int)luaL_optnumber(L, 5, 0), p);
    applyCanvas();
    if (fill) fillPolygon(p);
    else strokePolyline(p, true);
    return 0;
}

int w_ellipse(lua_State* L) {
    bool fill = isFill(L, 1);
    float x = (float)luaL_checknumber(L, 2), y = (float)luaL_checknumber(L, 3);
    float rx = (float)luaL_checknumber(L, 4), ry = (float)luaL_optnumber(L, 5, rx);
    Points p;
    ellipsePoints(x, y, rx, ry, (int)luaL_optnumber(L, 6, 0), p);
    applyCanvas();
    if (fill) fillPolygon(p);
    else strokePolyline(p, true);
    return 0;
}

int w_arc(lua_State* L) {
    bool fill = isFill(L, 1);
    int idx = 2;
    bool closed = true;
    if (lua_type(L, 2) == LUA_TSTRING) {
        closed = strcmp(lua_tostring(L, 2), "open") != 0;
        idx = 3;
    }
    float x = (float)luaL_checknumber(L, idx), y = (float)luaL_checknumber(L, idx + 1);
    float r = (float)luaL_checknumber(L, idx + 2);
    float a1 = (float)luaL_checknumber(L, idx + 3), a2 = (float)luaL_checknumber(L, idx + 4);
    int segs = (int)luaL_optnumber(L, idx + 5, 0);
    if (segs < 1) segs = 16;
    Points p;
    if (fill || closed) {
        p.push_back(x);
        p.push_back(y);
    }
    for (int i = 0; i <= segs; ++i) {
        float a = a1 + (a2 - a1) * i / segs;
        p.push_back(x + r * cosf(a));
        p.push_back(y + r * sinf(a));
    }
    applyCanvas();
    if (fill) fillPolygon(p);
    else strokePolyline(p, closed);
    return 0;
}

int w_points(lua_State* L) {
    Points p;
    readPoints(L, 1, p);
    applyCanvas();
    useTexture(g_white);
    float hs = g_state.pointSize * 0.5f;
    for (size_t i = 0; i + 1 < p.size(); i += 2) {
        float x, y;
        g_transform.apply(p[i], p[i + 1], x, y);
        Matrix id;
        addQuad(id, x - hs, y - hs, x + hs, y + hs, 0.5f, 0.5f, 0.5f, 0.5f, g_state.color);
    }
    return 0;
}

// -------------------------------------------------------------- Lua: misc
int w_getWidth(lua_State* L) {
    lua_pushinteger(L, g_dev->width());
    return 1;
}
int w_getHeight(lua_State* L) {
    lua_pushinteger(L, g_dev->height());
    return 1;
}
int w_getDimensions(lua_State* L) {
    lua_pushinteger(L, g_dev->width());
    lua_pushinteger(L, g_dev->height());
    return 2;
}
int w_getDPIScale(lua_State* L) {
    lua_pushnumber(L, 1);
    return 1;
}
int w_true(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}
int w_false(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}
int w_none(lua_State* L) {
    (void)L;
    return 0;
}

int w_getStats(lua_State* L) {
    lua_newtable(L);
    lua_pushinteger(L, g_drawCallsLast);
    lua_setfield(L, -2, "drawcalls");
    lua_pushinteger(L, g_canvasSwitchesLast);
    lua_setfield(L, -2, "canvasswitches");
    lua_pushinteger(L, g_verticesLast);
    lua_setfield(L, -2, "b360_vertices");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "texturememory");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "images");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "canvases");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "fonts");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "shaderswitches");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "drawcallsbatched");
    return 1;
}

// Extension: love.graphics._b360_setFrameRate(60 | 30 | "auto" | "vsync").
int w_setFrameRate(lua_State* L) {
    FramePacer::Mode m = FramePacer::MODE_VSYNC;
    if (lua_isnumber(L, 1)) {   // also "60" / "30"
        int fps = (int)lua_tonumber(L, 1);
        if (fps == 60) m = FramePacer::MODE_60;
        else if (fps == 30) m = FramePacer::MODE_30;
        else return luaL_error(L, "frame rate must be 60, 30 or \"auto\"");
    } else {
        const char* s = luaL_checkstring(L, 1);
        if (strcmp(s, "auto") == 0) m = FramePacer::MODE_AUTO;
        else if (strcmp(s, "vsync") != 0) return luaL_error(L, "frame rate must be 60, 30 or \"auto\"");
    }
    g_pacer.setMode(g_dev, m);
    return 0;
}

// Extension: love.graphics._b360_startProfiler(interval_ms, report_seconds).
int w_startProfiler(lua_State* L) {
    profilerStart(L, luaL_optnumber(L, 1, 1.0), luaL_optnumber(L, 2, 30.0));
    return 0;
}

// Extension: vblanks per frame (1 or 2), recent CPU ms per frame.
int w_getFrameInfo(lua_State* L) {
    lua_pushinteger(L, g_pacer.interval());
    lua_pushnumber(L, g_pacer.cpuMs());
    return 2;
}

// Extension: love.graphics._b360_setSafeArea(scale), scale in [0.5, 1].
int w_setSafeArea(lua_State* L) {
    float s = (float)luaL_checknumber(L, 1);
    if (s < 0.5f) s = 0.5f;
    if (s > 1.0f) s = 1.0f;
    flush();
    g_safeArea = s;
    return 0;
}

int w_getRendererInfo(lua_State* L) {
    lua_pushstring(L, "Balatro360");
    lua_pushstring(L, "1.0");
    lua_pushstring(L, "b360");
    lua_pushstring(L, g_dev->rendererName());
    return 4;
}

int w_trueTable(lua_State* L) {
    lua_newtable(L);
    const char* keys[] = {"clampzero", "lighten", "multicanvasformats", "glsl3", "instancing",
                          "fullnpot", "pixelshaderhighp", "shaderderivatives", NULL};
    for (int i = 0; keys[i]; ++i) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, keys[i]);
    }
    return 1;
}

int w_getSystemLimits(lua_State* L) {
    lua_newtable(L);
    lua_pushinteger(L, 4096);
    lua_setfield(L, -2, "texturesize");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "multicanvas");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "canvasmsaa");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "anisotropy");
    lua_pushinteger(L, 64);
    lua_setfield(L, -2, "pointsize");
    return 1;
}

const luaL_Reg kFuncs[] = {
    {"newImage", w_newImage}, {"newCanvas", w_newCanvas}, {"newQuad", w_newQuad},
    {"newShader", w_newShader}, {"newFont", w_newFont}, {"setNewFont", w_setNewFont},
    {"newText", w_newText}, {"newVideo", w_newVideo}, {"setFont", w_setFont}, {"getFont", w_getFont},
    {"setColor", w_setColor}, {"getColor", w_getColor}, {"setBackgroundColor", w_setBackgroundColor},
    {"getBackgroundColor", w_getBackgroundColor}, {"setShader", w_setShader}, {"getShader", w_getShader},
    {"setCanvas", w_setCanvas}, {"getCanvas", w_getCanvas}, {"clear", w_clear}, {"present", w_present},
    {"setBlendMode", w_setBlendMode}, {"getBlendMode", w_getBlendMode}, {"setScissor", w_setScissor},
    {"getScissor", w_getScissor}, {"intersectScissor", w_intersectScissor},
    {"setColorMask", w_setColorMask}, {"setLineWidth", w_setLineWidth}, {"getLineWidth", w_getLineWidth},
    {"setLineStyle", w_setLineStyle}, {"getLineStyle", w_getLineStyle}, {"setLineJoin", w_setLineJoin},
    {"getLineJoin", w_getLineJoin}, {"setPointSize", w_setPointSize}, {"getPointSize", w_getPointSize},
    {"setDefaultFilter", w_setDefaultFilter}, {"getDefaultFilter", w_getDefaultFilter},
    {"push", w_push}, {"pop", w_pop}, {"origin", w_origin}, {"translate", w_translate},
    {"scale", w_scale}, {"rotate", w_rotate}, {"shear", w_shear}, {"transformPoint", w_transformPoint},
    {"inverseTransformPoint", w_inverseTransformPoint}, {"getStackDepth", w_getStackDepth},
    {"reset", w_reset}, {"draw", w_draw}, {"print", w_print}, {"printf", w_printf},
    {"rectangle", w_rectangle}, {"polygon", w_polygon}, {"line", w_line}, {"circle", w_circle},
    {"ellipse", w_ellipse}, {"arc", w_arc}, {"points", w_points}, {"getWidth", w_getWidth},
    {"getHeight", w_getHeight}, {"getDimensions", w_getDimensions}, {"getPixelWidth", w_getWidth},
    {"getPixelHeight", w_getHeight}, {"getPixelDimensions", w_getDimensions},
    {"getDPIScale", w_getDPIScale}, {"isActive", w_true}, {"isCreated", w_true},
    {"isGammaCorrect", w_false}, {"isWireframe", w_false}, {"setWireframe", w_none},
    {"getStats", w_getStats}, {"getRendererInfo", w_getRendererInfo}, {"getSupported", w_trueTable},
    {"getSystemLimits", w_getSystemLimits}, {"getCanvasFormats", w_trueTable},
    {"getImageFormats", w_trueTable}, {"getTextureTypes", w_trueTable}, {"captureScreenshot", w_none},
    {"_b360_setSafeArea", w_setSafeArea}, {"_b360_setFrameRate", w_setFrameRate},
    {"_b360_getFrameInfo", w_getFrameInfo}, {"_b360_startProfiler", w_startProfiler}, {"setStencilTest", w_none}, {"stencil", w_none}, {"flushBatch", w_none}, {"discard", w_none},
    {NULL, NULL}};

}  // namespace

int graphicsWidth() { return g_dev ? g_dev->width() : 1280; }
int graphicsHeight() { return g_dev ? g_dev->height() : 720; }

bool initGraphics(int width, int height) {
    g_dev = gfx::createDevice();
    if (!g_dev || !g_dev->init(width, height)) return false;
    u8 white[4] = {255, 255, 255, 255};
    g_white = g_dev->createTexture(1, 1, white, false);
    resetState();
    return true;
}

void open_graphics(lua_State* L) {
    TypeDef drawable = {"Drawable", NULL, NULL};
    TypeDef texture = {"Texture", "Drawable", kTextureMethods};
    TypeDef image = {"Image", "Texture", NULL};
    TypeDef canvas = {"Canvas", "Texture", kCanvasMethods};
    TypeDef quad = {"Quad", NULL, kQuadMethods};
    TypeDef shader = {"Shader", NULL, kShaderMethods};
    TypeDef font = {"Font", NULL, kFontMethods};
    TypeDef text = {"Text", "Drawable", kTextMethods};
    luax_registertype(L, drawable);
    luax_registertype(L, texture);
    luax_registertype(L, image);
    luax_registertype(L, canvas);
    luax_registertype(L, quad);
    luax_registertype(L, shader);
    luax_registertype(L, font);
    luax_registertype(L, text);
    luax_registermodule(L, "graphics", kFuncs);
}

}  // namespace rt
