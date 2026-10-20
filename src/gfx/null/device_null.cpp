// Null rendering backend: accepts everything, draws nothing. Used for
// big-endian (PowerPC under qemu) test builds where no GPU API exists. It
// still runs the HLSL translator on every shader so translation problems
// show up in those builds.
#include "../device.h"

#include <stdlib.h>
#include <string.h>
#include "../shader_translate.h"
#include "../../platform/platform.h"

namespace gfx {

namespace {

class NullTexture : public Texture {};

class NullShader : public Shader {
public:
    virtual void setUniform(int, const float*, int) {}
    virtual void setUniformTexture(int, Texture*) {}
};

class NullDevice : public Device {
public:
    NullDevice() : w(0), h(0), interval(1), nextVblank(0), lastFlip(0) {
        const char* v = getenv("B360_NULL_VSYNC");
        vsync = v && strcmp(v, "1") == 0;
    }
    virtual bool init(int width, int height) {
        w = width;
        h = height;
        return true;
    }
    virtual int width() const { return w; }
    virtual int height() const { return h; }
    virtual Texture* createTexture(int tw, int th, const u8*, bool) {
        NullTexture* t = new NullTexture;
        t->width = tw;
        t->height = th;
        return t;
    }
    virtual Texture* createRenderTarget(int tw, int th) {
        Texture* t = createTexture(tw, th, NULL, false);
        t->renderTarget = true;
        return t;
    }
    virtual void updateTexture(Texture*, int, int, int, int, const u8*) {}
    virtual void destroyTexture(Texture* t) { delete t; }
    virtual void setTextureParams(Texture* t, Filter mi, Filter ma, Wrap ws, Wrap wt) {
        t->minFilter = mi;
        t->magFilter = ma;
        t->wrapS = ws;
        t->wrapT = wt;
    }
    virtual Shader* createShader(const std::string& source, std::string& err) {
        (void)err;
        LoveShaderInfo info;
        analyzeLoveShader(source, info);
        std::vector<std::string> warnings;
        buildHLSLStage(source, info, true, &warnings);
        buildHLSLStage(source, info, false, &warnings);
        for (size_t i = 0; i < warnings.size(); ++i) plat::log("[null] shader warning: %s", warnings[i].c_str());
        NullShader* s = new NullShader;
        s->uniforms = info.uniforms;
        return s;
    }
    virtual void destroyShader(Shader* s) { delete s; }
    virtual void setRenderTarget(Texture*) {}
    virtual void clear(float, float, float, float) {}
    virtual void setScissor(bool, int, int, int, int) {}
    virtual void setColorMask(bool, bool, bool, bool) {}
    virtual void drawTriangles(const Vertex*, int, Texture*, Shader*, BlendMode) {}
    // B360_NULL_VSYNC=1 emulates a 60 Hz display (present() waits for the
    // next vblank, or the one after at interval 2), for testing frame pacing.
    virtual void present() {
        if (!vsync) return;
        const double period = 1.0 / 60.0;
        double now = plat::getTime();
        if (nextVblank == 0) nextVblank = now;
        double target = lastFlip + interval * period;
        while (nextVblank < now || nextVblank < target - 0.0001) nextVblank += period;
        plat::sleep(nextVblank - now);
        lastFlip = nextVblank;
    }
    virtual bool setPresentInterval(int n) {
        interval = n;
        return vsync;
    }
    virtual const char* rendererName() const { return "null"; }

private:
    int w, h;
    bool vsync;
    int interval;
    double nextVblank, lastFlip;
};

}  // namespace

Device* createDevice() { return new NullDevice; }

}  // namespace gfx
