// Null rendering backend: accepts everything, draws nothing. Used for
// big-endian (PowerPC under qemu) test builds where no GPU API exists. It
// still runs the HLSL translator on every shader so translation problems
// show up in those builds.
#include "../device.h"
#include "../shader_translate.h"
#include "../../platform/platform.h"

namespace gfx {

namespace {

class NullTexture : public Texture {};

class NullShader : public Shader {
public:
    virtual void setUniform(const std::string&, const float*, int) {}
    virtual void setUniformTexture(const std::string&, Texture*) {}
};

class NullDevice : public Device {
public:
    NullDevice() : w(0), h(0) {}
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
    virtual void present() {}
    virtual const char* rendererName() const { return "null"; }

private:
    int w, h;
};

}  // namespace

Device* createDevice() { return new NullDevice; }

}  // namespace gfx
