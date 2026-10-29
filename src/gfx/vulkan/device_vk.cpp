// Vulkan 1.0 backend: vulkan4xenon on the Xbox 360 (the xSDK build), or any
// Vulkan driver on the Linux host, where it renders offscreen for tests.
//
// How the 2D core maps onto Vulkan:
//  * Every frame records one command buffer; vulkan4xenon replays it into
//    the GPU's ring at vkQueueSubmit. Two frames are in flight, each with its
//    own region of a vertex ring, a uniform ring and a staging ring.
//  * The back buffer and every canvas are B8G8R8A8 render targets drawn in
//    render passes that load, clear or ignore what they held before;
//    vulkan4xenon keeps them in eDRAM and resolves them into their images.
//  * Images and fonts are R8G8B8A8 optimal-tiled images filled with
//    vkCmdCopyBufferToImage from staging memory whose rows are laid out as
//    vulkan4xenon draws such copies on the GPU (no wait for the GPU).
//  * Shaders are LÖVE GLSL translated to Vulkan GLSL (shader_translate.cpp),
//    compiled to SPIR-V with glslang at load time and, on the console, by
//    vulkan4xenon into Xenos microcode, which it keeps in game:\cache.
//  * A shader's uniforms are one std140 block, written into the uniform ring
//    when they change and read at a dynamic offset.
#include "../device.h"
#include "../shader_translate.h"
#include "../../platform/platform.h"
#include "glsl_spirv.h"

#include <vulkan/vulkan.h>
#ifdef __XSDK__
#include <vulkan4xenon.h>
#include <xsdk/present.h>
#endif

#include <algorithm>
#include <map>
#include <stdlib.h>
#include <string.h>

// In C++98 on 64-bit hosts VK_NULL_HANDLE is ((void*)0), which C++ won't
// turn into a handle; 0 becomes any handle, pointer or 64-bit integer.
#undef VK_NULL_HANDLE
#define VK_NULL_HANDLE 0

namespace gfx {

namespace {

const int kFrames = 2;
// Per frame, in GPU memory (vulkan4xenon budgets 256 MB for everything); a
// frame that fills one submits what it has and waits for the GPU.
const VkDeviceSize kVertexRing = 4 * 1024 * 1024;    // 131072 vertices
const VkDeviceSize kUniformRing = 1024 * 1024;       // 4096 uniform blocks
const VkDeviceSize kStagingRing = 8 * 1024 * 1024;   // texture uploads
const VkDeviceSize kUniformAlign = 256;              // minUniformBufferOffsetAlignment
const VkDeviceSize kUniformRange = 2048;             // the most a shader's block may use
const VkFormat kTargetFormat = VK_FORMAT_B8G8R8A8_UNORM;
const VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;
const u32 kFrameStamps = 1024;                       // time stamps a GPU-profiled frame takes at most

#ifdef __XSDK__
// GCC turns small copies into 8-byte stores, which fault in the GPU's
// write-combined memory unless 8-byte aligned; xSDK's memcpy keeps to the
// stores that memory takes (xsdk docs/PITFALLS.md), so call it, never inline.
void* (*volatile g_memcpy)(void*, const void*, size_t) = memcpy;
inline void gpuCopy(void* dst, const void* src, size_t n) { g_memcpy(dst, src, n); }
#else
inline void gpuCopy(void* dst, const void* src, size_t n) { memcpy(dst, src, n); }
#endif

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) & ~(a - 1); }

bool vkOk(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    plat::log("[vk] %s failed: %d", what, (int)r);
    return false;
}

struct Buffer {
    VkBuffer buf;
    VkDeviceMemory mem;
    u8* map;
    VkDeviceSize size;
    Buffer() : buf(VK_NULL_HANDLE), mem(VK_NULL_HANDLE), map(NULL), size(0) {}
};

class VkTex : public Texture {
public:
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkFramebuffer fb;              // render targets
    VkImageLayout layout;          // after the commands recorded so far
    bool swapchainImage;
    u32 levels;                    // mip levels
    bool zeroInit;                 // created blank: updates go through shadow
    std::vector<u8> shadow;        // CPU copy for 8-pixel-aligned updates
    std::map<int, VkDescriptorSet> sets;   // by sampler key
    VkTex()
        : image(VK_NULL_HANDLE), mem(VK_NULL_HANDLE), view(VK_NULL_HANDLE), fb(VK_NULL_HANDLE),
          layout(VK_IMAGE_LAYOUT_UNDEFINED), swapchainImage(false), levels(1), zeroInit(false) {}
};

class VkShaderObj : public Shader {
public:
    VkShaderModule vs, fs;
    VkShaderModule fsNoIfs;        // the pixel stage without its uniform ifs, or none
    std::vector<std::vector<VkLoopGuard> > uniformIfs;   // their guards
    std::vector<int> offsets;      // std140 offset of each uniform
    std::vector<u8> block;         // the uniform block, builtins first
    int blockBytes;
    std::vector<VkHardwareLoop> loops;   // the pixel stage's, their ints from loopAt
    int loopAt;
    bool dirty;
    u32 uploadedSerial;            // frame the block was last written in
    VkDeviceSize uploadedOffset;
    int uploadedW, uploadedH;
    bool uploadedHalf;
    std::map<u32, VkPipeline> pipelines;   // by blend mode, colour mask and variant

    VkShaderObj()
        : vs(VK_NULL_HANDLE), fs(VK_NULL_HANDLE), fsNoIfs(VK_NULL_HANDLE), blockBytes(VK_BUILTIN_UNIFORM_BYTES),
          loopAt(0), dirty(true), uploadedSerial(0), uploadedOffset(0), uploadedW(0), uploadedH(0),
          uploadedHalf(false) {}

    // A guard against the uniform's value now (a uniform without a place in
    // the block holds).
    bool holds(const VkLoopGuard& g) const {
        int at = g.uniform < (int)offsets.size() ? offsets[g.uniform] : -1;
        if (at < 0 || at + 4 > (int)block.size()) return true;
        float v;
        memcpy(&v, &block[at], 4);
        switch (g.op) {
        case VkLoopGuard::GT: return v > g.value;
        case VkLoopGuard::GE: return v >= g.value;
        case VkLoopGuard::LT: return v < g.value;
        default: return v <= g.value;
        }
    }

    bool allHold(const std::vector<VkLoopGuard>& guards) const {
        for (size_t g = 0; g < guards.size(); ++g)
            if (!holds(guards[g])) return false;
        return true;
    }

    // Whether every uniform if fails, so the variant without them draws the same.
    bool uniformIfsFail() const {
        if (!fsNoIfs) return false;
        for (size_t k = 0; k < uniformIfs.size(); ++k)
            if (allHold(uniformIfs[k])) return false;
        return true;
    }

    // Each loop's int: its count, or its start (no passes) where a guard
    // fails, as the console runs the if around it for every pixel.
    void putLoopCounts() {
        for (size_t k = 0; k < loops.size(); ++k) {
            const VkHardwareLoop& l = loops[k];
            int n = allHold(l.guards) ? l.count : l.start;
            int at = loopAt + 4 * (int)k;
            if (at + 4 <= (int)block.size()) memcpy(&block[at], &n, 4);
        }
    }

    virtual void setUniform(int index, const float* v, int count) {
        if (index < 0 || index >= (int)uniforms.size() || offsets[index] < 0) return;
        const UniformInfo& u = uniforms[index];
        int at = offsets[index];
        int n = u.arraySize < 1 ? 1 : u.arraySize;
        if (u.type == UNIFORM_MAT) {
            // Column-major values, a column per vec4 (std140).
            int dim = u.components;
            for (int e = 0; e < n; ++e)
                for (int c = 0; c < dim; ++c)
                    for (int r = 0; r < dim; ++r) {
                        int src = (e * dim + c) * dim + r;
                        if (src < count) put(at + (e * dim + c) * 16 + r * 4, v[src]);
                    }
        } else {
            int stride = n > 1 ? 16 : 4 * u.components;
            for (int e = 0; e < n; ++e)
                for (int c = 0; c < u.components; ++c) {
                    int src = e * u.components + c;
                    if (src < count) put(at + e * stride + c * 4, v[src]);
                }
        }
        dirty = true;
    }
    virtual void setUniformTexture(int index, Texture* tex) {
        (void)index;
        (void)tex;   // Balatro only samples the main texture.
    }

private:
    void put(int at, float f) {
        if (at >= 0 && at + 4 <= (int)block.size()) memcpy(&block[at], &f, 4);
    }
};

#ifdef __XSDK__
// 64-bit FNV-1a, for the SPIR-V cache's file names.
u64 fnv1a(const std::string& s) {
    u64 h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < s.size(); ++i) h = (h ^ (u8)s[i]) * 0x100000001B3ULL;
    return h;
}

std::string hex64(u64 v) {
    static const char* d = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, v >>= 4) s[i] = d[v & 15];
    return s;
}
#endif

struct Frame {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired, rendered;
    std::vector<Buffer> spareStaging;   // big uploads, freed when the frame is done
    std::vector<VkTex*> deadTextures;
    std::vector<VkShaderObj*> deadShaders;
    std::vector<std::string> stamps;    // a GPU-profiled frame's time stamps: what each one ends
    Frame() : cmd(VK_NULL_HANDLE), fence(VK_NULL_HANDLE), acquired(VK_NULL_HANDLE), rendered(VK_NULL_HANDLE) {}
};

class VkDevice_ : public Device {
public:
    VkDevice_()
        : w(0), h(0), instance(VK_NULL_HANDLE), phys(VK_NULL_HANDLE), dev(VK_NULL_HANDLE), queue(VK_NULL_HANDLE),
          queueFamily(0), surface(VK_NULL_HANDLE), swapchain(VK_NULL_HANDLE), imageIndex(0), headless(true),
          offscreen(NULL), cmdPool(VK_NULL_HANDLE), setLayout(VK_NULL_HANDLE), pipeLayout(VK_NULL_HANDLE),
          frame(0), frameStarted(false), serial(1), acquired(false), acquireWaited(false), target(NULL),
          passOpen(false), pendingClear(false), passTex(NULL), vertexUsed(0), uniformUsed(0), stagingUsed(0),
          boundPipeline(VK_NULL_HANDLE), boundSet(VK_NULL_HANDLE), boundOffset(~(VkDeviceSize)0),
          defaultShader(NULL), scissorOn(false), sx(0), sy(0), sw(0), sh(0), maskBits(15), interval(1),
          lastQueueVblank(0), halfLayer(NULL), halfResDrawing(false), halfFullW(0), halfFullH(0), captureTex(NULL),
          captureWanted(false), captureFrame(false),
          queryPool(VK_NULL_HANDLE), tickNs(1.0f), gpuEvery(0.0), gpuLast(0.0), profiling(false), cpuFence(0.0),
          cpuAcquire(0.0), cpuSubmit(0.0), cpuPresent(0.0), cpuFrames(0) {
        memset(clearColor, 0, sizeof(clearColor));
        memset(samplers, 0, sizeof(samplers));
    }

    // ------------------------------------------------------------- set-up
    virtual bool init(int width, int height) {
        w = width;
        h = height;
        if (!createInstance() || !pickDevice() || !createDevice()) return false;
#ifdef __XSDK__
        // vulkan4xenon compiles SPIR-V on the console the first time it sees
        // it; keep what it compiled for the next start-up.
        plat::dirCreate("game:/cache/xenos");
        v4x_set_shader_cache("game:/cache/xenos");
#endif
        if (!createCommon()) return false;
        headless = !createSwapchain();
        if (headless) {
            plat::log("[vk] no display: drawing offscreen");
            offscreen = makeTarget(w, h, true);
            if (!offscreen) return false;
        }
        std::string err;
        defaultShader = (VkShaderObj*)createShader("", err);
        if (!defaultShader) {
            plat::log("[vk] default shader failed:\n%s", err.c_str());
            return false;
        }
        defaultShader->name = "default";
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys, &props);
        tickNs = props.limits.timestampComputeAndGraphics ? props.limits.timestampPeriod : 0.0f;
        plat::log("[vk] %s, %dx%d%s", props.deviceName, w, h, headless ? " (offscreen)" : "");
        return true;
    }

    virtual int width() const { return w; }
    virtual int height() const { return h; }

    // ------------------------------------------------------------ textures
    virtual Texture* createTexture(int tw, int th, const u8* pixels, bool mipmaps) {
        VkTex* t = new VkTex;
        t->width = tw;
        t->height = th;
        if (mipmaps && pixels)
            while ((tw >> t->levels) > 0 || (th >> t->levels) > 0) ++t->levels;
        if (!makeImage(t, kTextureFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            delete t;
            return NULL;
        }
        t->zeroInit = true;
        if (pixels) {
            size_t n = (size_t)tw * th * 4;
            for (size_t i = 0; i < n; ++i)
                if (pixels[i]) {
                    t->zeroInit = false;
                    break;
                }
        }
        if (pixels && !t->zeroInit) {
            upload(t, 0, 0, tw, th, pixels, tw * 4, 0);
            if (t->levels > 1) uploadMips(t, pixels);
        } else {
            // Blank: clear it, and keep updates (font glyphs) in a shadow copy.
            endPass();
            beginFrame();
            barrier(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue c;
            memset(&c, 0, sizeof(c));
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->levels, 0, 1};
            vkCmdClearColorImage(frames[frame].cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
            barrier(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        return t;
    }

    virtual Texture* createRenderTarget(int tw, int th) { return makeTarget(tw, th, false); }

    virtual void updateTexture(Texture* tex, int x, int y, int tw, int th, const u8* pixels) {
        VkTex* t = (VkTex*)tex;
        if (!t || t->renderTarget || tw <= 0 || th <= 0) return;
        if (!t->zeroInit) {
            // Level 0 alone: Balatro updates only its (unmipmapped) font pages.
            upload(t, x, y, tw, th, pixels, tw * 4, 0);
            return;
        }
        // vulkan4xenon draws a copy on the GPU, without waiting for it to be
        // done with the image, when its rectangle is on the 8-pixel grid; so
        // grow the rectangle to it and fill the rest from the shadow copy.
        if (t->shadow.empty()) t->shadow.assign((size_t)t->width * t->height * 4, 0);
        for (int r = 0; r < th; ++r)
            memcpy(&t->shadow[((size_t)(y + r) * t->width + x) * 4], pixels + (size_t)r * tw * 4, (size_t)tw * 4);
        int x0 = x & ~7, y0 = y & ~7;
        int x1 = (x + tw + 7) & ~7, y1 = (y + th + 7) & ~7;
        if (x1 > t->width) x1 = t->width;
        if (y1 > t->height) y1 = t->height;
        upload(t, x0, y0, x1 - x0, y1 - y0, &t->shadow[((size_t)y0 * t->width + x0) * 4], t->width * 4, 0);
    }

    virtual void destroyTexture(Texture* tex) {
        VkTex* t = (VkTex*)tex;
        if (!t) return;
        // Freed once the GPU is done with the frame being recorded.
        beginFrame();
        if (target == t) setRenderTarget(NULL);
        if (passTex == t) endPass();
        for (std::map<int, VkDescriptorSet>::iterator it = t->sets.begin(); it != t->sets.end(); ++it)
            if (it->second == boundSet) boundSet = VK_NULL_HANDLE;
        frames[frame].deadTextures.push_back(t);
    }

    virtual void setTextureParams(Texture* tex, Filter minF, Filter magF, Wrap ws, Wrap wt) {
        tex->minFilter = minF;
        tex->magFilter = magF;
        tex->wrapS = ws;
        tex->wrapT = wt;
    }

    // ------------------------------------------------------------- shaders
    virtual Shader* createShader(const std::string& source, std::string& err) {
        double t0 = plat::getTime();
        LoveShaderInfo info;
        analyzeLoveShader(source, info);
        VkShaderObj* s = new VkShaderObj;
        s->uniforms = info.uniforms;
        if (!buildModules(s, source, info, err)) {
            if (!defaultShader) {
                delete s;
                return NULL;
            }
            // Draw it like the default shader rather than stop the game; its
            // uniforms still take values.
            plat::log("[vk] shader failed, drawn with the default shader instead:\n%s", err.c_str());
            err.clear();
            LoveShaderInfo none;
            analyzeLoveShader("", none);
            std::string unused;
            if (!buildModules(s, "", none, unused)) {
                delete s;
                return NULL;
            }
            s->offsets.assign(info.uniforms.size(), -1);
        }
        plat::log("[vk] shader compiled in %.0f ms", (plat::getTime() - t0) * 1000.0);
        return s;
    }

    virtual void destroyShader(Shader* sh) {
        VkShaderObj* s = (VkShaderObj*)sh;
        if (!s) return;
        beginFrame();
        for (std::map<u32, VkPipeline>::iterator it = s->pipelines.begin(); it != s->pipelines.end(); ++it)
            if (it->second == boundPipeline) boundPipeline = VK_NULL_HANDLE;
        frames[frame].deadShaders.push_back(s);
    }

    // ------------------------------------------------------------- targets
    virtual void setRenderTarget(Texture* rt) {
        VkTex* t = (VkTex*)rt;
        if (t == target) return;
        endPass();
        target = t;
    }

    virtual void clear(float r, float g, float b, float a) {
        float c[4] = {r, g, b, a};
        if (passOpen || scissorOn) {
            ensurePass();
            VkClearAttachment att;
            att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            att.colorAttachment = 0;
            memcpy(att.clearValue.color.float32, c, sizeof(c));
            VkClearRect rect;
            rect.rect = currentScissor();
            rect.baseArrayLayer = 0;
            rect.layerCount = 1;
            if (rect.rect.extent.width && rect.rect.extent.height)
                vkCmdClearAttachments(frames[frame].cmd, 1, &att, 1, &rect);
            return;
        }
        // Not drawn into yet: the pass that begins it clears it.
        memcpy(clearColor, c, sizeof(c));
        pendingClear = true;
    }

    virtual void setScissor(bool enabled, int x, int y, int sw_, int sh_) {
        scissorOn = enabled;
        sx = x;
        sy = y;
        sw = sw_;
        sh = sh_;
        if (passOpen) {
            VkRect2D r = currentScissor();
            vkCmdSetScissor(frames[frame].cmd, 0, 1, &r);
        }
    }

    virtual void setColorMask(bool r, bool g, bool b, bool a) {
        maskBits = (r ? 1u : 0u) | (g ? 2u : 0u) | (b ? 4u : 0u) | (a ? 8u : 0u);
    }

    // ------------------------------------------------------------- drawing
    virtual void drawTriangles(const Vertex* verts, int count, Texture* tex, Shader* shader, BlendMode blend) {
        if (count <= 0) return;
        VkDeviceSize bytes = (VkDeviceSize)count * sizeof(Vertex);
        if (bytes > kVertexRing) return;
        VkShaderObj* s = shader ? (VkShaderObj*)shader : defaultShader;
        if (s->halfRes && !halfResDrawing && target && blend == BLEND_ALPHA && !scissorOn && maskBits == 15 &&
            target->width >= 2 && target->height >= 2 && drawHalfRes(verts, count, tex, s, blend))
            return;
        ensurePass();
        VkDeviceSize need = alignUp((VkDeviceSize)s->blockBytes, kUniformAlign);
        if (vertexUsed + bytes > kVertexRing || uniformUsed + need > kUniformRing) {
            flushAndWait();
            ensurePass();
        }
        VkCommandBuffer cmd = frames[frame].cmd;
        VkPipeline p = pipeline(s, blend, s->uniformIfsFail());
        if (p == VK_NULL_HANDLE) return;
        if (p != boundPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            boundPipeline = p;
        }
        VkDeviceSize offset = uniforms(s);
        VkDescriptorSet set = descriptorSet((VkTex*)tex);
        if (set == VK_NULL_HANDLE) return;
        if (set != boundSet || offset != boundOffset) {
            u32 dyn = (u32)offset;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &set, 1, &dyn);
            boundSet = set;
            boundOffset = offset;
        }
        u32 first = (u32)(vertexUsed / sizeof(Vertex));
        gpuCopy(vertexRing.map + frame * kVertexRing + vertexUsed, verts, (size_t)bytes);
        vertexUsed += bytes;
        vkCmdDraw(cmd, (u32)count, 1, first, 0);
        if (profiling) stamp(s->name + (halfResDrawing ? " (half size) -> " : " -> ") + targetName(passTex));
    }

    // A shader marked halfRes (backgrounds that are blocks of two pixels or
    // more anyway) draws into a target of half the width and height, which
    // is then drawn over the canvas, nearest: a fourth of the pixels. For
    // alpha blending that's what drawing straight in gives, as the layer
    // holds the colour premultiplied by alpha. False if there's no layer.
    bool drawHalfRes(const Vertex* verts, int count, Texture* tex, VkShaderObj* s, BlendMode blend) {
        VkTex* full = target;
        int lw = full->width / 2, lh = full->height / 2;
        if (!halfLayer || halfLayer->width != lw || halfLayer->height != lh) {
            beginFrame();
            if (halfLayer) frames[frame].deadTextures.push_back(halfLayer);
            halfLayer = makeTarget(lw, lh, false);
            if (!halfLayer) return false;
            halfLayer->minFilter = halfLayer->magFilter = FILTER_NEAREST;
        }
        if (passOpen) endPass();   // what the canvas drew so far; a clear not begun yet stays for after
        bool pending = pendingClear;
        float colour[4];
        memcpy(colour, clearColor, sizeof(colour));
        target = halfLayer;
        memset(clearColor, 0, sizeof(clearColor));
        pendingClear = true;
        halfResDrawing = true;
        halfFullW = full->width;
        halfFullH = full->height;
        drawTriangles(verts, count, tex, s, blend);
        endPass();
        halfResDrawing = false;
        target = full;
        pendingClear = pending;
        memcpy(clearColor, colour, sizeof(colour));
        float fw = (float)full->width, fh = (float)full->height;
        Vertex q[6] = {{0, 0, 0, 0, 1, 1, 1, 1},   {fw, 0, 1, 0, 1, 1, 1, 1},  {fw, fh, 1, 1, 1, 1, 1, 1},
                       {0, 0, 0, 0, 1, 1, 1, 1},   {fw, fh, 1, 1, 1, 1, 1, 1}, {0, fh, 0, 1, 1, 1, 1, 1}};
        drawTriangles(q, 6, halfLayer, defaultShader, BLEND_ALPHA_PREMUL);
        return true;
    }

    virtual void present() {
        beginFrame();
        finishCapture(false);
        if (!headless && !acquired) {
            // Nothing drew to the screen this frame: show it cleared.
            setRenderTarget(NULL);
            const float black[4] = {0, 0, 0, 1};
            memcpy(clearColor, black, sizeof(black));
            pendingClear = true;
            ensurePass();
        }
        setRenderTarget(NULL);
        endPass();
        Frame& f = frames[frame];
        if (profiling) stamp("end of frame");
        profiling = false;
        vkEndCommandBuffer(f.cmd);
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        bool waitAcquire = acquired && !acquireWaited;
        si.waitSemaphoreCount = waitAcquire ? 1 : 0;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = acquired ? 1 : 0;
        si.pSignalSemaphores = &f.rendered;
        double t0 = gpuEvery > 0 ? plat::getTime() : 0.0;
        vkOk(vkQueueSubmit(queue, 1, &si, f.fence), "vkQueueSubmit");
        double t1 = gpuEvery > 0 ? plat::getTime() : 0.0;
        if (gpuEvery > 0) {
            cpuSubmit += t1 - t0;
            ++cpuFrames;
        }
        if (acquired) {
            paceBeforePresent();
            VkPresentInfoKHR pi;
            memset(&pi, 0, sizeof(pi));
            pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &f.rendered;
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain;
            pi.pImageIndices = &imageIndex;
            vkOk(vkQueuePresentKHR(queue, &pi), "vkQueuePresentKHR");
            if (gpuEvery > 0) cpuPresent += plat::getTime() - t1;
        }
        acquired = false;
        acquireWaited = false;
        frameStarted = false;
        frame = (frame + 1) % kFrames;
    }

    virtual bool setPresentInterval(int vblanks) {
#ifdef __XSDK__
        if (headless) return false;
        interval = vblanks >= 2 ? 2 : 1;
        return true;
#else
        (void)vblanks;
        return false;
#endif
    }

    // Offscreen, the frame's image; on a display, a frame captureNextFrame()
    // drew into an image of its own.
    virtual bool readBackbuffer(std::vector<u8>& rgba) {
        if (!headless && !captureFrame && !(captureWanted && !acquired)) return false;
        VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
        if (readback.size < bytes) {
            destroyBuffer(readback);
            if (!makeBuffer(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
        }
        beginFrame();
        if (headless) {
            setRenderTarget(NULL);
            ensurePass();   // the frame may not have drawn to it
            endPass();
            copyToReadback(offscreen);
        } else {
            if (!captureFrame) {   // nothing drew to the screen yet
                setRenderTarget(NULL);
                ensurePass();
            }
            if (!finishCapture(true)) return false;
        }
        flushAndWait();
        // One copy out of mapped memory, which the console doesn't cache.
        rgba.resize((size_t)bytes);
        memcpy(&rgba[0], readback.map, (size_t)bytes);
        for (size_t i = 0; i < (size_t)w * h; ++i) {   // BGRA -> RGBA
            u8 b = rgba[i * 4 + 0];
            rgba[i * 4 + 0] = rgba[i * 4 + 2];
            rgba[i * 4 + 2] = b;
            if (!headless) rgba[i * 4 + 3] = 255;   // as the TV shows it
        }
        return true;
    }

    virtual bool captureNextFrame() {
        if (headless) return false;
        captureWanted = true;
        return true;
    }

    virtual bool setGpuProfile(double seconds) {
        if (seconds <= 0) {
            gpuEvery = 0;
            return true;
        }
        if (tickNs <= 0) return false;
        if (!queryPool) {
            VkQueryPoolCreateInfo qi;
            memset(&qi, 0, sizeof(qi));
            qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = kFrames * kFrameStamps;
            if (!vkOk(vkCreateQueryPool(dev, &qi, NULL, &queryPool), "vkCreateQueryPool")) return false;
        }
        gpuEvery = seconds;
        gpuLast = plat::getTime();
        plat::log("[gpu] timing one frame every %.0f s", seconds);
        return true;
    }

#ifdef __XSDK__
    virtual const char* rendererName() const { return "Vulkan (vulkan4xenon)"; }
#else
    virtual const char* rendererName() const { return "Vulkan"; }
#endif

private:
    int w, h;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    u32 queueFamily;
    VkPhysicalDeviceMemoryProperties memProps;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    std::vector<VkTex*> swapImages;
    u32 imageIndex;
    bool headless;
    VkTex* offscreen;
    VkCommandPool cmdPool;
    VkDescriptorSetLayout setLayout;
    VkPipelineLayout pipeLayout;
    std::vector<VkDescriptorPool> descPools;
    std::map<VkDescriptorSet, VkDescriptorPool> setPools;
    std::map<std::pair<int, std::pair<int, int> >, VkRenderPass> renderPasses;
    VkRenderPass compatPass;   // what pipelines and framebuffers are made with
    VkSampler samplers[72];
    Frame frames[kFrames];
    int frame;
    bool frameStarted;
    u32 serial;                // counts frames, for the uniform uploads
    bool acquired, acquireWaited;
    VkTex* target;             // NULL: the back buffer
    bool passOpen;
    bool pendingClear;
    float clearColor[4];
    VkTex* passTex;            // the image the open pass draws into
    Buffer vertexRing, uniformRing, stagingRing, readback;
    VkDeviceSize vertexUsed, uniformUsed, stagingUsed;
    VkPipeline boundPipeline;
    VkDescriptorSet boundSet;
    VkDeviceSize boundOffset;
    VkShaderObj* defaultShader;
    bool scissorOn;
    int sx, sy, sw, sh;
    u32 maskBits;
    int interval;
    u32 lastQueueVblank;
    // Half-size drawing (drawHalfRes): the layer, and the full target's size.
    VkTex* halfLayer;
    bool halfResDrawing;
    int halfFullW, halfFullH;
    // Screenshots on a display (captureNextFrame): the frame draws the screen
    // into captureTex, which is read back, then drawn to the swapchain image.
    VkTex* captureTex;
    bool captureWanted, captureFrame;
    // GPU timing (setGpuProfile): one frame every gpuEvery seconds takes a
    // time stamp after each draw, pass begin and pass end, read back once its
    // fence signals; the CPU's waits are summed for the same log.
    VkQueryPool queryPool;
    float tickNs;
    double gpuEvery, gpuLast;
    bool profiling;
    double cpuFence, cpuAcquire, cpuSubmit, cpuPresent;
    int cpuFrames;

    // ------------------------------------------------------------ helpers
    bool createInstance() {
        std::vector<const char*> exts;
        u32 n = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
        std::vector<VkExtensionProperties> props(n);
        if (n) vkEnumerateInstanceExtensionProperties(NULL, &n, &props[0]);
        bool hasSurface = false, hasDisplay = false;
        for (u32 i = 0; i < n; ++i) {
            if (!strcmp(props[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME)) hasSurface = true;
            if (!strcmp(props[i].extensionName, VK_KHR_DISPLAY_EXTENSION_NAME)) hasDisplay = true;
        }
        if (hasSurface && hasDisplay) {
            exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            exts.push_back(VK_KHR_DISPLAY_EXTENSION_NAME);
        }
        VkApplicationInfo app;
        memset(&app, 0, sizeof(app));
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Balatro360";
        app.pEngineName = "Balatro360";
        app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = (u32)exts.size();
        ci.ppEnabledExtensionNames = exts.empty() ? NULL : &exts[0];
#ifndef __XSDK__
        // Host tests: B360_VK_VALIDATE=1 turns on Khronos's validation layer.
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* validate = getenv("B360_VK_VALIDATE");
        if (validate && validate[0] == '1') {
            ci.enabledLayerCount = 1;
            ci.ppEnabledLayerNames = &layer;
        }
#endif
        return vkOk(vkCreateInstance(&ci, NULL, &instance), "vkCreateInstance");
    }

    bool pickDevice() {
        u32 n = 0;
        vkEnumeratePhysicalDevices(instance, &n, NULL);
        if (!n) {
            plat::log("[vk] no Vulkan device");
            return false;
        }
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance, &n, &devs[0]);
        phys = devs[0];
        vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, NULL);
        std::vector<VkQueueFamilyProperties> qf(qn);
        if (qn) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, &qf[0]);
        for (u32 i = 0; i < qn; ++i)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queueFamily = i;
                return true;
            }
        plat::log("[vk] no graphics queue");
        return false;
    }

    bool createDevice() {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi;
        memset(&qi, 0, sizeof(qi));
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = queueFamily;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        u32 n = 0;
        vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
        std::vector<VkExtensionProperties> props(n);
        if (n) vkEnumerateDeviceExtensionProperties(phys, NULL, &n, &props[0]);
        std::vector<const char*> exts;
        for (u32 i = 0; i < n; ++i)
            if (!strcmp(props[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        VkDeviceCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qi;
        ci.enabledExtensionCount = (u32)exts.size();
        ci.ppEnabledExtensionNames = exts.empty() ? NULL : &exts[0];
        if (!vkOk(vkCreateDevice(phys, &ci, NULL, &dev), "vkCreateDevice")) return false;
        vkGetDeviceQueue(dev, queueFamily, 0, &queue);
        return true;
    }

    bool createSwapchain() {
#ifdef __XSDK__
        u32 n = 0;
        if (vkGetPhysicalDeviceDisplayPropertiesKHR(phys, &n, NULL) != VK_SUCCESS || !n) return false;
        std::vector<VkDisplayPropertiesKHR> displays(n);
        vkGetPhysicalDeviceDisplayPropertiesKHR(phys, &n, &displays[0]);
        u32 m = 0;
        vkGetDisplayModePropertiesKHR(phys, displays[0].display, &m, NULL);
        if (!m) return false;
        std::vector<VkDisplayModePropertiesKHR> modes(m);
        vkGetDisplayModePropertiesKHR(phys, displays[0].display, &m, &modes[0]);
        VkDisplaySurfaceCreateInfoKHR sci;
        memset(&sci, 0, sizeof(sci));
        sci.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
        sci.displayMode = modes[0].displayMode;
        sci.planeIndex = 0;
        sci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        sci.globalAlpha = 1.0f;
        sci.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
        sci.imageExtent = modes[0].parameters.visibleRegion;
        if (!vkOk(vkCreateDisplayPlaneSurfaceKHR(instance, &sci, NULL, &surface), "vkCreateDisplayPlaneSurfaceKHR"))
            return false;
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
        VkSwapchainCreateInfoKHR ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface;
        ci.minImageCount = caps.minImageCount > 2 ? caps.minImageCount : 2;
        ci.imageFormat = kTargetFormat;
        ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        ci.imageExtent.width = (u32)w;
        ci.imageExtent.height = (u32)h;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped = VK_TRUE;
        if (!vkOk(vkCreateSwapchainKHR(dev, &ci, NULL, &swapchain), "vkCreateSwapchainKHR")) return false;
        u32 count = 0;
        vkGetSwapchainImagesKHR(dev, swapchain, &count, NULL);
        std::vector<VkImage> images(count);
        vkGetSwapchainImagesKHR(dev, swapchain, &count, &images[0]);
        for (u32 i = 0; i < count; ++i) {
            VkTex* t = new VkTex;
            t->width = w;
            t->height = h;
            t->renderTarget = true;
            t->swapchainImage = true;
            t->image = images[i];
            if (!makeView(t, kTargetFormat) || !makeFramebuffer(t)) return false;
            swapImages.push_back(t);
        }
        return true;
#else
        return false;
#endif
    }

    bool createCommon() {
        VkCommandPoolCreateInfo pci;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily;
        if (!vkOk(vkCreateCommandPool(dev, &pci, NULL, &cmdPool), "vkCreateCommandPool")) return false;
        for (int i = 0; i < kFrames; ++i) {
            VkCommandBufferAllocateInfo ai;
            memset(&ai, 0, sizeof(ai));
            ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ai.commandPool = cmdPool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (!vkOk(vkAllocateCommandBuffers(dev, &ai, &frames[i].cmd), "vkAllocateCommandBuffers")) return false;
            VkFenceCreateInfo fi;
            memset(&fi, 0, sizeof(fi));
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(dev, &fi, NULL, &frames[i].fence);
            VkSemaphoreCreateInfo si;
            memset(&si, 0, sizeof(si));
            si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(dev, &si, NULL, &frames[i].acquired);
            vkCreateSemaphore(dev, &si, NULL, &frames[i].rendered);
        }
        if (!makeBuffer(vertexRing, kVertexRing * kFrames, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
            // The descriptor's range reaches kUniformRange past the last block.
            !makeBuffer(uniformRing, kUniformRing * kFrames + kUniformRange, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
            !makeBuffer(stagingRing, kStagingRing * kFrames, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return false;

        VkDescriptorSetLayoutBinding b[2];
        memset(b, 0, sizeof(b));
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li;
        memset(&li, 0, sizeof(li));
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings = b;
        if (!vkOk(vkCreateDescriptorSetLayout(dev, &li, NULL, &setLayout), "vkCreateDescriptorSetLayout"))
            return false;
        VkPipelineLayoutCreateInfo pli;
        memset(&pli, 0, sizeof(pli));
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout;
        if (!vkOk(vkCreatePipelineLayout(dev, &pli, NULL, &pipeLayout), "vkCreatePipelineLayout")) return false;
        compatPass = renderPass(VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return compatPass != VK_NULL_HANDLE;
    }

    u32 memoryType(u32 bits, VkMemoryPropertyFlags want) {
        for (u32 i = 0; i < memProps.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return i;
        for (u32 i = 0; i < memProps.memoryTypeCount; ++i)
            if (bits & (1u << i)) return i;
        return 0;
    }

    bool makeBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage) {
        VkBufferCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size = size;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vkOk(vkCreateBuffer(dev, &ci, NULL, &b.buf), "vkCreateBuffer")) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, b.buf, &req);
        VkMemoryAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!vkOk(vkAllocateMemory(dev, &ai, NULL, &b.mem), "vkAllocateMemory (buffer)")) return false;
        vkBindBufferMemory(dev, b.buf, b.mem, 0);
        void* p = NULL;
        if (!vkOk(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory")) return false;
        b.map = (u8*)p;
        b.size = size;
        return true;
    }

    void destroyBuffer(Buffer& b) {
        if (b.buf) vkDestroyBuffer(dev, b.buf, NULL);
        if (b.mem) vkFreeMemory(dev, b.mem, NULL);
        b = Buffer();
    }

    bool makeImage(VkTex* t, VkFormat format, VkImageUsageFlags usage) {
        VkImageCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent.width = (u32)t->width;
        ci.extent.height = (u32)t->height;
        ci.extent.depth = 1;
        ci.mipLevels = t->levels;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!vkOk(vkCreateImage(dev, &ci, NULL, &t->image), "vkCreateImage")) {
            plat::log("[vk] image %dx%d", t->width, t->height);
            return false;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, t->image, &req);
        VkMemoryAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!vkOk(vkAllocateMemory(dev, &ai, NULL, &t->mem), "vkAllocateMemory (image)")) {
            plat::log("[vk] image %dx%d", t->width, t->height);
            vkDestroyImage(dev, t->image, NULL);
            return false;
        }
        vkBindImageMemory(dev, t->image, t->mem, 0);
        return makeView(t, format);
    }

    bool makeView(VkTex* t, VkFormat format) {
        VkImageViewCreateInfo vi;
        memset(&vi, 0, sizeof(vi));
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = t->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = t->levels;
        vi.subresourceRange.layerCount = 1;
        return vkOk(vkCreateImageView(dev, &vi, NULL, &t->view), "vkCreateImageView");
    }

    bool makeFramebuffer(VkTex* t) {
        VkFramebufferCreateInfo fi;
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = compatPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &t->view;
        fi.width = (u32)t->width;
        fi.height = (u32)t->height;
        fi.layers = 1;
        return vkOk(vkCreateFramebuffer(dev, &fi, NULL, &t->fb), "vkCreateFramebuffer");
    }

    VkTex* makeTarget(int tw, int th, bool backbuffer) {
        VkTex* t = new VkTex;
        t->width = tw;
        t->height = th;
        t->renderTarget = true;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  (backbuffer ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!makeImage(t, kTargetFormat, usage) || !makeFramebuffer(t)) {
            delete t;
            return NULL;
        }
        if (!backbuffer) {
            // LÖVE's canvases start transparent; one may be drawn before it is
            // drawn into. The current target keeps a clear it has pending.
            VkTex* saved = target;
            bool pending = pendingClear;
            float colour[4];
            memcpy(colour, clearColor, sizeof(colour));
            if (passOpen) endPass();
            target = t;
            memset(clearColor, 0, sizeof(clearColor));
            pendingClear = true;
            ensurePass();
            endPass();
            target = saved;
            pendingClear = pending;
            memcpy(clearColor, colour, sizeof(colour));
        }
        return t;
    }

    void freeTexture(VkTex* t) {
        for (std::map<int, VkDescriptorSet>::iterator it = t->sets.begin(); it != t->sets.end(); ++it)
            freeSet(it->second);
        if (t->fb) vkDestroyFramebuffer(dev, t->fb, NULL);
        if (t->view) vkDestroyImageView(dev, t->view, NULL);
        if (!t->swapchainImage) {
            if (t->image) vkDestroyImage(dev, t->image, NULL);
            if (t->mem) vkFreeMemory(dev, t->mem, NULL);
        }
        delete t;
    }

    void freeShader(VkShaderObj* s) {
        for (std::map<u32, VkPipeline>::iterator it = s->pipelines.begin(); it != s->pipelines.end(); ++it)
            vkDestroyPipeline(dev, it->second, NULL);
        if (s->vs) vkDestroyShaderModule(dev, s->vs, NULL);
        if (s->fs) vkDestroyShaderModule(dev, s->fs, NULL);
        if (s->fsNoIfs) vkDestroyShaderModule(dev, s->fsNoIfs, NULL);
        delete s;
    }

    // -------------------------------------------------------------- frames
    void beginFrame() {
        if (frameStarted) return;
        Frame& f = frames[frame];
        double t0 = gpuEvery > 0 ? plat::getTime() : 0.0;
        vkWaitForFences(dev, 1, &f.fence, VK_TRUE, ~(u64)0);
        if (gpuEvery > 0) cpuFence += plat::getTime() - t0;
        vkResetFences(dev, 1, &f.fence);
        releaseGarbage(f);
        if (!f.stamps.empty()) logGpuFrame(f);
        vertexUsed = uniformUsed = stagingUsed = 0;
        ++serial;
        beginCommands();
        frameStarted = true;
        if (gpuEvery > 0 && plat::getTime() - gpuLast >= gpuEvery) {
            gpuLast = plat::getTime();
            profiling = true;
            vkCmdResetQueryPool(f.cmd, queryPool, (u32)frame * kFrameStamps, kFrameStamps);
            stamp("start of frame");
        }
    }

    // A time stamp once the GPU has done everything before it.
    void stamp(const std::string& what) {
        Frame& f = frames[frame];
        if (!profiling || f.stamps.size() >= kFrameStamps) return;
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool,
                            (u32)frame * kFrameStamps + (u32)f.stamps.size());
        f.stamps.push_back(what);
    }

    std::string targetName(const VkTex* t) const {
        if (!t || t == offscreen || t == captureTex || t->swapchainImage) return "screen";
        char buf[32];
        int n = 0, v[2] = {t->width, t->height};
        const char* head = "canvas ";
        while (*head) buf[n++] = *head++;
        for (int k = 0; k < 2; ++k) {
            char d[12];
            int m = 0, x = v[k];
            do {
                d[m++] = (char)('0' + x % 10);
                x /= 10;
            } while (x > 0 && m < 11);
            while (m > 0) buf[n++] = d[--m];
            if (k == 0) buf[n++] = 'x';
        }
        return std::string(buf, n);
    }

    // The profiled frame's time between stamps, summed by what each ends,
    // longest first; then the CPU's waits per frame since the last log.
    void logGpuFrame(Frame& f) {
        u32 n = (u32)f.stamps.size();
        std::vector<u64> ts(n);
        VkResult r = vkGetQueryPoolResults(dev, queryPool, (u32)frame * kFrameStamps, n, n * sizeof(u64), &ts[0],
                                           sizeof(u64), VK_QUERY_RESULT_64_BIT);
        if (r == VK_SUCCESS && n >= 2) {
            std::map<std::string, std::pair<double, int> > by;
            std::map<std::string, double> longest;
            for (u32 i = 1; i < n; ++i) {
                double ms = ts[i] >= ts[i - 1] ? (double)(ts[i] - ts[i - 1]) * tickNs / 1e6 : 0.0;
                std::pair<double, int>& e = by[f.stamps[i]];
                e.first += ms;
                ++e.second;
                if (ms > longest[f.stamps[i]]) longest[f.stamps[i]] = ms;
            }
            std::vector<std::pair<double, std::string> > order;
            for (std::map<std::string, std::pair<double, int> >::iterator it = by.begin(); it != by.end(); ++it)
                order.push_back(std::make_pair(it->second.first, it->first));
            std::sort(order.begin(), order.end());
            plat::log("[gpu] one frame: %.2f ms on the GPU, %u time stamps (each waits for the GPU)",
                      (double)(ts[n - 1] - ts[0]) * tickNs / 1e6, n);
            for (size_t i = order.size(), shown = 0; i-- > 0 && shown < 24; ++shown) {
                const std::pair<double, int>& e = by[order[i].second];
                plat::log("[gpu] %8.2f ms %4d x  %s (longest %.2f ms)", e.first, e.second, order[i].second.c_str(),
                          longest[order[i].second]);
            }
        } else {
            plat::log("[gpu] time stamps not read (%d)", (int)r);
        }
        f.stamps.clear();
        if (cpuFrames > 0) {
            plat::log("[gpu] CPU per frame over %d frames: fence wait %.2f ms, image wait %.2f ms, submit %.2f ms, "
                      "pacing and present %.2f ms",
                      cpuFrames, cpuFence * 1000 / cpuFrames, cpuAcquire * 1000 / cpuFrames,
                      cpuSubmit * 1000 / cpuFrames, cpuPresent * 1000 / cpuFrames);
            cpuFence = cpuAcquire = cpuSubmit = cpuPresent = 0;
            cpuFrames = 0;
        }
    }

    void beginCommands() {
        Frame& f = frames[frame];
        vkResetCommandBuffer(f.cmd, 0);
        VkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof(bi));
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);
        VkDeviceSize vo = (VkDeviceSize)frame * kVertexRing;
        vkCmdBindVertexBuffers(f.cmd, 0, 1, &vertexRing.buf, &vo);
        boundPipeline = VK_NULL_HANDLE;
        boundSet = VK_NULL_HANDLE;
        boundOffset = ~(VkDeviceSize)0;
    }

    void releaseGarbage(Frame& f) {
        for (size_t i = 0; i < f.spareStaging.size(); ++i) destroyBuffer(f.spareStaging[i]);
        f.spareStaging.clear();
        for (size_t i = 0; i < f.deadTextures.size(); ++i) freeTexture(f.deadTextures[i]);
        f.deadTextures.clear();
        for (size_t i = 0; i < f.deadShaders.size(); ++i) freeShader(f.deadShaders[i]);
        f.deadShaders.clear();
    }

    // Submits what's recorded and waits for it, then goes on recording the
    // same frame: for full rings and read-backs.
    void flushAndWait() {
        beginFrame();
        endPass();
        Frame& f = frames[frame];
        vkEndCommandBuffer(f.cmd);
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        bool waitAcquire = acquired && !acquireWaited;
        si.waitSemaphoreCount = waitAcquire ? 1 : 0;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        vkOk(vkQueueSubmit(queue, 1, &si, f.fence), "vkQueueSubmit (flush)");
        if (waitAcquire) acquireWaited = true;
        vkWaitForFences(dev, 1, &f.fence, VK_TRUE, ~(u64)0);
        vkResetFences(dev, 1, &f.fence);
        releaseGarbage(f);
        vertexUsed = uniformUsed = stagingUsed = 0;
        ++serial;
        beginCommands();
    }

    void paceBeforePresent() {
#ifdef __XSDK__
        // vulkan4xenon's swapchain shows each frame for one vblank; at 30 fps,
        // queue a frame two vblanks after the last one at the soonest.
        XsdkPresentStats st;
        xsdk_present_stats(&st);
        if (interval >= 2) {
            double start = plat::getTime();
            while (st.vblanks < lastQueueVblank + 2 && plat::getTime() - start < 0.05) {
                plat::sleep(0.001);
                xsdk_present_stats(&st);
            }
        }
        lastQueueVblank = st.vblanks;
#endif
    }

    // --------------------------------------------------------- render passes
    VkRenderPass renderPass(VkAttachmentLoadOp load, VkImageLayout initial, VkImageLayout final) {
        std::pair<int, std::pair<int, int> > key((int)load, std::make_pair((int)initial, (int)final));
        std::map<std::pair<int, std::pair<int, int> >, VkRenderPass>::iterator it = renderPasses.find(key);
        if (it != renderPasses.end()) return it->second;
        VkAttachmentDescription a;
        memset(&a, 0, sizeof(a));
        a.format = kTargetFormat;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = load;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = initial;
        a.finalLayout = final;
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub;
        memset(&sub, 0, sizeof(sub));
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency deps[2];
        memset(deps, 0, sizeof(deps));
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &a;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        VkRenderPass rp = VK_NULL_HANDLE;
        vkOk(vkCreateRenderPass(dev, &ci, NULL, &rp), "vkCreateRenderPass");
        renderPasses[key] = rp;
        return rp;
    }

    VkTex* backbuffer() {
        if (headless) return offscreen;
        if (captureWanted && !acquired && !captureFrame) {
            captureWanted = false;
            if (!captureTex) captureTex = makeCaptureTarget();
            if (captureTex) {
                captureFrame = true;
                captureTex->layout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
        }
        if (captureFrame) return captureTex;
        if (!acquired) {
            beginFrame();
            Frame& f = frames[frame];
            double t0 = gpuEvery > 0 ? plat::getTime() : 0.0;
            VkResult r = vkAcquireNextImageKHR(dev, swapchain, ~(u64)0, f.acquired, VK_NULL_HANDLE, &imageIndex);
            if (gpuEvery > 0) cpuAcquire += plat::getTime() - t0;
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                vkOk(r, "vkAcquireNextImageKHR");
                return NULL;
            }
            acquired = true;
            acquireWaited = false;
            swapImages[imageIndex]->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        return swapImages[imageIndex];
    }

    void ensurePass() {
        if (passOpen) return;
        beginFrame();
        VkTex* t = target ? target : backbuffer();
        if (!t) return;
        VkAttachmentLoadOp load = pendingClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                  : t->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                                           : VK_ATTACHMENT_LOAD_OP_LOAD;
        VkImageLayout final = target ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                              : headless || t == captureTex ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                            : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkRenderPass rp = renderPass(load, t->layout, final);
        VkClearValue cv;
        memcpy(cv.color.float32, clearColor, sizeof(clearColor));
        VkRenderPassBeginInfo bi;
        memset(&bi, 0, sizeof(bi));
        bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass = rp;
        bi.framebuffer = t->fb;
        bi.renderArea.extent.width = (u32)t->width;
        bi.renderArea.extent.height = (u32)t->height;
        bi.clearValueCount = 1;
        bi.pClearValues = &cv;
        VkCommandBuffer cmd = frames[frame].cmd;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        t->layout = final;   // once the pass ends
        passTex = t;
        passOpen = true;
        pendingClear = false;
        VkViewport vp = {0, 0, (float)t->width, (float)t->height, 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc = currentScissor();
        vkCmdSetScissor(cmd, 0, 1, &sc);
        boundPipeline = VK_NULL_HANDLE;
        boundSet = VK_NULL_HANDLE;
        if (profiling) {
            const char* how = load == VK_ATTACHMENT_LOAD_OP_CLEAR ? "clear"
                              : load == VK_ATTACHMENT_LOAD_OP_LOAD ? "load (drawn back)"
                                                                   : "begin";
            stamp(std::string("pass ") + how + " " + targetName(t));
        }
    }

    void endPass() {
        if (!passOpen && pendingClear) ensurePass();   // a clear nothing drew over
        if (!passOpen) return;
        vkCmdEndRenderPass(frames[frame].cmd);
        if (profiling) stamp("pass end (resolved) " + targetName(passTex));
        passOpen = false;
        passTex = NULL;
    }

    // ---------------------------------------------------------- screenshots
    VkTex* makeCaptureTarget() {
        VkTex* t = new VkTex;
        t->width = w;
        t->height = h;
        t->renderTarget = true;
        VkImageUsageFlags usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!makeImage(t, kTargetFormat, usage) || !makeFramebuffer(t)) {
            delete t;
            return NULL;
        }
        t->minFilter = t->magFilter = FILTER_NEAREST;
        return t;
    }

    void copyToReadback(VkTex* t) {
        VkBufferImageCopy region;
        memset(&region, 0, sizeof(region));
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = (u32)w;
        region.imageExtent.height = (u32)h;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(frames[frame].cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buf, 1,
                               &region);
    }

    // Ends a capture frame's screen pass: copies the image out (copy), then
    // draws it to the swapchain image so the frame still shows.
    bool finishCapture(bool copy) {
        if (!captureFrame) return false;
        setRenderTarget(NULL);
        if (captureTex->layout == VK_IMAGE_LAYOUT_UNDEFINED || pendingClear) ensurePass();   // not drawn to yet
        endPass();
        if (copy) copyToReadback(captureTex);
        barrier(captureTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        captureFrame = false;
        bool scissor = scissorOn;
        u32 mask = maskBits;
        scissorOn = false;
        maskBits = 15;
        float fw = (float)w, fh = (float)h;
        Vertex q[6] = {{0, 0, 0, 0, 1, 1, 1, 1},   {fw, 0, 1, 0, 1, 1, 1, 1},  {fw, fh, 1, 1, 1, 1, 1, 1},
                       {0, 0, 0, 0, 1, 1, 1, 1},   {fw, fh, 1, 1, 1, 1, 1, 1}, {0, fh, 0, 1, 1, 1, 1, 1}};
        pendingClear = false;
        drawTriangles(q, 6, captureTex, NULL, BLEND_REPLACE);
        endPass();
        scissorOn = scissor;
        maskBits = mask;
        return true;
    }

    VkRect2D currentScissor() const {
        VkTex* t = target;
        int tw = t ? t->width : w, th = t ? t->height : h;
        int x0 = 0, y0 = 0, x1 = tw, y1 = th;
        if (scissorOn) {
            x0 = sx > 0 ? sx : 0;
            y0 = sy > 0 ? sy : 0;
            x1 = sx + sw < tw ? sx + sw : tw;
            y1 = sy + sh < th ? sy + sh : th;
            if (x1 < x0) x1 = x0;
            if (y1 < y0) y1 = y0;
        }
        VkRect2D r;
        r.offset.x = x0;
        r.offset.y = y0;
        r.extent.width = (u32)(x1 - x0);
        r.extent.height = (u32)(y1 - y0);
        return r;
    }

    // ------------------------------------------------------------- uploads
    void barrier(VkTex* t, VkImageLayout to) {
        if (t->layout == to) return;
        VkImageMemoryBarrier b;
        memset(&b, 0, sizeof(b));
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT
                                                                     : VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = t->layout;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = t->levels;
        b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(frames[frame].cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        t->layout = to;
    }

    // Copies rows of RGBA8 into the image. Copies run outside render passes,
    // so an open one ends and the next draw loads it back.
    // Each level from the one above it, a 2x2 box filter on straight RGBA, as
    // glGenerateMipmap makes them on the host build.
    void uploadMips(VkTex* t, const u8* base) {
        std::vector<u8> prev(base, base + (size_t)t->width * t->height * 4), next;
        int pw = t->width, ph = t->height;
        for (u32 level = 1; level < t->levels; ++level) {
            int lw = pw > 1 ? pw / 2 : 1, lh = ph > 1 ? ph / 2 : 1;
            next.resize((size_t)lw * lh * 4);
            for (int y = 0; y < lh; ++y) {
                int y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = y * 2 + 1 < ph ? y * 2 + 1 : ph - 1;
                for (int x = 0; x < lw; ++x) {
                    int x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = x * 2 + 1 < pw ? x * 2 + 1 : pw - 1;
                    const u8* a = &prev[((size_t)y0 * pw + x0) * 4];
                    const u8* b = &prev[((size_t)y0 * pw + x1) * 4];
                    const u8* c = &prev[((size_t)y1 * pw + x0) * 4];
                    const u8* d = &prev[((size_t)y1 * pw + x1) * 4];
                    u8* o = &next[((size_t)y * lw + x) * 4];
                    for (int k = 0; k < 4; ++k) o[k] = (u8)((a[k] + b[k] + c[k] + d[k] + 2) >> 2);
                }
            }
            upload(t, 0, 0, lw, lh, &next[0], lw * 4, level);
            prev.swap(next);
            pw = lw;
            ph = lh;
        }
    }

    void upload(VkTex* t, int x, int y, int uw, int uh, const u8* rgba, int stride, u32 level) {
        if (uw <= 0 || uh <= 0) return;
        endPass();
        beginFrame();
        // Rows spaced as xsdk spaces a linear texture's (texels rounded up to
        // 32, bytes to 256) at a 4 KB-aligned address, so vulkan4xenon draws
        // the copy on the GPU (its SUPPORT.md, Commands).
        u32 pitchTexels = (u32)(alignUp(alignUp((VkDeviceSize)uw, 32) * 4, 256) / 4);
        VkDeviceSize bytes = (VkDeviceSize)pitchTexels * 4 * uh;
        VkBuffer src = stagingRing.buf;
        u8* dst = NULL;
        VkDeviceSize offset = 0;
        if (bytes > kStagingRing) {
            Buffer big;
            if (!makeBuffer(big, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return;
            frames[frame].spareStaging.push_back(big);
            src = big.buf;
            dst = big.map;
        } else {
            if (alignUp(stagingUsed, 4096) + bytes > kStagingRing) flushAndWait();
            stagingUsed = alignUp(stagingUsed, 4096);
            offset = (VkDeviceSize)frame * kStagingRing + stagingUsed;
            dst = stagingRing.map + offset;
            stagingUsed += bytes;
        }
        for (int r = 0; r < uh; ++r) gpuCopy(dst + (size_t)r * pitchTexels * 4, rgba + (size_t)r * stride, (size_t)uw * 4);
        barrier(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region;
        memset(&region, 0, sizeof(region));
        region.bufferOffset = offset;
        region.bufferRowLength = pitchTexels;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = level;
        region.imageSubresource.layerCount = 1;
        region.imageOffset.x = x;
        region.imageOffset.y = y;
        region.imageExtent.width = (u32)uw;
        region.imageExtent.height = (u32)uh;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(frames[frame].cmd, src, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // ------------------------------------------------------------- shaders
    bool spirv(bool vertex, const std::string& glsl, std::vector<u32>& out, std::string& err) {
#ifdef __XSDK__
        // glslang takes about a second to set up on the console, so keep what
        // it made: a file per GLSL source, named after its hash.
        std::string path = "game:/cache/spirv/" + hex64(fnv1a(glsl)) + (vertex ? ".vs" : ".fs");
        std::vector<u8> bytes;
        if (plat::fileRead(path, bytes) && bytes.size() >= 20 && bytes.size() % 4 == 0) {
            out.resize(bytes.size() / 4);
            memcpy(&out[0], &bytes[0], bytes.size());
            if (out[0] == 0x07230203u) return true;
        }
#endif
        std::string log;
        if (!glslToSpirv(vertex, glsl, out, log)) {
            err += vertex ? "vertex: " : "pixel: ";
            err += log;
            return false;
        }
#ifdef __XSDK__
        plat::dirCreate("game:/cache/spirv");
        plat::fileWrite(path, &out[0], out.size() * 4, false);
#endif
        return true;
    }

    bool module(const std::vector<u32>& code, VkShaderModule& m) {
        VkShaderModuleCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size() * 4;
        ci.pCode = &code[0];
        return vkCreateShaderModule(dev, &ci, NULL, &m) == VK_SUCCESS;
    }

    bool buildModules(VkShaderObj* s, const std::string& source, const LoveShaderInfo& info, std::string& err) {
        std::vector<std::string> warnings;
        std::string vsrc = buildVulkanGLSLStage(source, info, true, &warnings);
        std::string fsrc = buildVulkanGLSLStage(source, info, false, &warnings, &s->loops, &s->uniformIfs);
        for (size_t i = 0; i < warnings.size(); ++i) plat::log("[vk] shader warning: %s", warnings[i].c_str());
        std::vector<u32> vcode, fcode;
        bool ok = spirv(true, vsrc, vcode, err);
        ok = spirv(false, fsrc, fcode, err) && ok;
        if (!ok) return false;
        if (!module(vcode, s->vs)) {
            err += "vertex: vkCreateShaderModule failed (the log above has the compiler's reason)\n";
            return false;
        }
        if (!module(fcode, s->fs)) {
            vkDestroyShaderModule(dev, s->vs, NULL);
            s->vs = VK_NULL_HANDLE;
            err += "pixel: vkCreateShaderModule failed (the log above has the compiler's reason)\n";
            return false;
        }
        vulkanUniformLayout(info, s->offsets, s->blockBytes);
        s->loopAt = s->blockBytes;
        s->blockBytes = vulkanLoopBlockBytes(s->blockBytes, (int)s->loops.size());
        if (s->blockBytes > (int)kUniformRange) {
            err += "uniforms too large\n";
            return false;
        }
        s->block.assign(s->blockBytes, 0);
        if (!s->uniformIfs.empty()) {
            // The variant drawn while every uniform if fails (CRT with bloom
            // and glitches off: a fifth fewer instructions a pixel).
            std::string fsrcNoIfs = buildVulkanGLSLStage(source, info, false, NULL, NULL, NULL, true);
            std::vector<u32> code;
            std::string why;
            if (!spirv(false, fsrcNoIfs, code, why) || !module(code, s->fsNoIfs)) {
                plat::log("[vk] the variant without uniform ifs failed, drawing the whole shader:\n%s", why.c_str());
                s->fsNoIfs = VK_NULL_HANDLE;
            }
        }
        return true;
    }

    VkPipeline pipeline(VkShaderObj* s, BlendMode blend, bool noIfs) {
        u32 key = (u32)blend | (maskBits << 8) | (noIfs ? 1u << 16 : 0u);
        std::map<u32, VkPipeline>::iterator it = s->pipelines.find(key);
        if (it != s->pipelines.end()) return it->second;

        VkPipelineShaderStageCreateInfo st[2];
        memset(st, 0, sizeof(st));
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = s->vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = noIfs ? s->fsNoIfs : s->fs;
        st[1].pName = "main";

        VkVertexInputBindingDescription vb = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription va[3] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},          // position
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},   // colour
            {2, 0, VK_FORMAT_R32G32_SFLOAT, 8}};         // texcoord
        VkPipelineVertexInputStateCreateInfo vi;
        memset(&vi, 0, sizeof(vi));
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions = va;
        VkPipelineInputAssemblyStateCreateInfo ia;
        memset(&ia, 0, sizeof(ia));
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps;
        memset(&vps, 0, sizeof(vps));
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs;
        memset(&rs, 0, sizeof(rs));
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms;
        memset(&ms, 0, sizeof(ms));
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba;
        memset(&ba, 0, sizeof(ba));
        blendState(blend, ba);
        ba.colorWriteMask = maskBits;   // R=1, G=2, B=4, A=8, as Vulkan's bits
        VkPipelineColorBlendStateCreateInfo cb;
        memset(&cb, 0, sizeof(cb));
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds;
        memset(&ds, 0, sizeof(ds));
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vps;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &ds;
        ci.layout = pipeLayout;
        ci.renderPass = compatPass;
        VkPipeline p = VK_NULL_HANDLE;
        if (!vkOk(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p), "vkCreateGraphicsPipelines"))
            p = VK_NULL_HANDLE;
        s->pipelines[key] = p;
        return p;
    }

    static void blendState(BlendMode b, VkPipelineColorBlendAttachmentState& a) {
        VkBlendOp op = VK_BLEND_OP_ADD, opA = VK_BLEND_OP_ADD;
        VkBlendFactor src, dst, srcA, dstA;
        switch (b) {
        case BLEND_ALPHA:
            src = VK_BLEND_FACTOR_SRC_ALPHA;
            dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            srcA = VK_BLEND_FACTOR_ONE;
            dstA = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BLEND_ALPHA_PREMUL:
            src = VK_BLEND_FACTOR_ONE;
            dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            srcA = VK_BLEND_FACTOR_ONE;
            dstA = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BLEND_ADD:
            src = VK_BLEND_FACTOR_SRC_ALPHA;
            dst = VK_BLEND_FACTOR_ONE;
            srcA = VK_BLEND_FACTOR_ZERO;
            dstA = VK_BLEND_FACTOR_ONE;
            break;
        case BLEND_SUBTRACT:
            op = opA = VK_BLEND_OP_REVERSE_SUBTRACT;
            src = VK_BLEND_FACTOR_SRC_ALPHA;
            dst = VK_BLEND_FACTOR_ONE;
            srcA = VK_BLEND_FACTOR_ZERO;
            dstA = VK_BLEND_FACTOR_ONE;
            break;
        case BLEND_MULTIPLY:
            src = VK_BLEND_FACTOR_DST_COLOR;
            dst = VK_BLEND_FACTOR_ZERO;
            srcA = VK_BLEND_FACTOR_DST_ALPHA;
            dstA = VK_BLEND_FACTOR_ZERO;
            break;
        case BLEND_SCREEN:
            src = VK_BLEND_FACTOR_ONE;
            dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            srcA = VK_BLEND_FACTOR_ONE;
            dstA = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            break;
        case BLEND_REPLACE:
        default:
            a.blendEnable = VK_FALSE;
            return;
        }
        a.blendEnable = VK_TRUE;
        a.colorBlendOp = op;
        a.alphaBlendOp = opA;
        a.srcColorBlendFactor = src;
        a.dstColorBlendFactor = dst;
        a.srcAlphaBlendFactor = srcA;
        a.dstAlphaBlendFactor = dstA;
    }

    // Writes the shader's block into the uniform ring when it changed, or
    // hasn't been written since the ring was reset, or the target size
    // changed; returns its offset. The caller made room.
    VkDeviceSize uniforms(VkShaderObj* s) {
        VkTex* t = target ? target : passTex;
        int tw = t ? t->width : w, th = t ? t->height : h;
        if (halfResDrawing) {   // the full target's pixels and size, drawn into its half-size layer
            tw = halfFullW;
            th = halfFullH;
        }
        if (!s->dirty && s->uploadedSerial == serial && s->uploadedW == tw && s->uploadedH == th &&
            s->uploadedHalf == halfResDrawing)
            return s->uploadedOffset;
        VkDeviceSize need = alignUp((VkDeviceSize)s->blockBytes, kUniformAlign);
        // Pixels (y down) to Vulkan's clip space (y down), column-major; then
        // love_ScreenSize and b360_FragCoordMap, which takes a half-size
        // layer's pixel centres to the top left of each two by two.
        float half = halfResDrawing ? 2.0f : 1.0f;
        float builtins[24] = {2.0f / tw, 0, 0, 0,  0, 2.0f / th, 0, 0,  0, 0, 1, 0,  -1, -1, 0, 1,
                              (float)tw, (float)th, 1.0f, 0.0f,  half, half, 0.5f - 0.5f * half, 0.5f - 0.5f * half};
        memcpy(&s->block[0], builtins, sizeof(builtins));
        s->putLoopCounts();
        VkDeviceSize offset = (VkDeviceSize)frame * kUniformRing + uniformUsed;
        gpuCopy(uniformRing.map + offset, &s->block[0], s->block.size());
        uniformUsed += need;
        s->dirty = false;
        s->uploadedSerial = serial;
        s->uploadedOffset = offset;
        s->uploadedW = tw;
        s->uploadedH = th;
        s->uploadedHalf = halfResDrawing;
        return offset;
    }

    VkSampler sampler(const VkTex* t) {
        int key = samplerKey(t);
        if (samplers[key]) return samplers[key];
        VkSamplerCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.minFilter = t->minFilter == FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        ci.magFilter = t->magFilter == FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        // Mipmapped images filter between levels too (LÖVE's default).
        ci.mipmapMode = t->levels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = addressMode(t->wrapS);
        ci.addressModeV = addressMode(t->wrapT);
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod = t->levels > 1 ? 15.0f : 0.0f;   // every level (vulkan4xenon takes up to 15)
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        vkOk(vkCreateSampler(dev, &ci, NULL, &samplers[key]), "vkCreateSampler");
        return samplers[key];
    }

    // The sampler's settings; a mipmapped image's sampler reads its levels.
    static int samplerKey(const VkTex* t) {
        int key = (((int)t->minFilter * 2 + (int)t->magFilter) * 3 + (int)t->wrapS) * 3 + (int)t->wrapT;
        return key * 2 + (t->levels > 1 ? 1 : 0);
    }

    static VkSamplerAddressMode addressMode(Wrap wr) {
        if (wr == WRAP_REPEAT) return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (wr == WRAP_MIRROR) return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }

    VkDescriptorSet descriptorSet(VkTex* t) {
        int key = samplerKey(t);
        std::map<int, VkDescriptorSet>::iterator it = t->sets.find(key);
        if (it != t->sets.end()) return it->second;
        VkDescriptorSet set = allocSet();
        if (set == VK_NULL_HANDLE) return set;
        VkDescriptorBufferInfo bi = {uniformRing.buf, 0, kUniformRange};
        VkDescriptorImageInfo ii = {sampler(t), t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr[2];
        memset(wr, 0, sizeof(wr));
        wr[0].sType = wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = wr[1].dstSet = set;
        wr[0].dstBinding = 0;
        wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[0].pBufferInfo = &bi;
        wr[1].dstBinding = 1;
        wr[1].descriptorCount = 1;
        wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[1].pImageInfo = &ii;
        vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);
        t->sets[key] = set;
        return set;
    }

    VkDescriptorSet allocSet() {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!descPools.empty()) {
                VkDescriptorSetAllocateInfo ai;
                memset(&ai, 0, sizeof(ai));
                ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                ai.descriptorPool = descPools.back();
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &setLayout;
                VkDescriptorSet set = VK_NULL_HANDLE;
                if (vkAllocateDescriptorSets(dev, &ai, &set) == VK_SUCCESS) {
                    setPools[set] = descPools.back();
                    return set;
                }
            }
            VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024},
                                             {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024}};
            VkDescriptorPoolCreateInfo pi;
            memset(&pi, 0, sizeof(pi));
            pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            pi.maxSets = 1024;
            pi.poolSizeCount = 2;
            pi.pPoolSizes = sizes;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (!vkOk(vkCreateDescriptorPool(dev, &pi, NULL, &pool), "vkCreateDescriptorPool")) return VK_NULL_HANDLE;
            descPools.push_back(pool);
        }
        return VK_NULL_HANDLE;
    }

    void freeSet(VkDescriptorSet set) {
        std::map<VkDescriptorSet, VkDescriptorPool>::iterator it = setPools.find(set);
        if (it == setPools.end()) return;
        vkFreeDescriptorSets(dev, it->second, 1, &set);
        setPools.erase(it);
    }
};

}  // namespace

Device* createDevice() { return new VkDevice_; }

}  // namespace gfx
