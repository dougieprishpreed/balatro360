// OpenGL 2.1 backend for the Linux host build, rendering offscreen through
// OSMesa. The back buffer is itself a texture-backed FBO so that every
// render target shares one orientation (row 0 = top, see device.h).
#define GL_GLEXT_PROTOTYPES 1
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "../device.h"
#include "../shader_translate.h"
#include "../../platform/platform.h"

#include <map>
#include <string.h>

namespace gfx {

namespace {

GLenum glFilter(Filter f) { return f == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR; }
GLenum glWrap(Wrap w) {
    switch (w) {
    case WRAP_REPEAT: return GL_REPEAT;
    case WRAP_MIRROR: return GL_MIRRORED_REPEAT;
    default: return GL_CLAMP_TO_EDGE;
    }
}

class GLTexture : public Texture {
public:
    GLuint id;
    GLuint fbo;
    bool mipmapped;
    GLTexture() : id(0), fbo(0), mipmapped(false) {}
};

struct GLUniformValue {
    std::vector<float> data;
    bool dirty;
};

class GLShader : public Shader {
public:
    GLuint program;
    GLint locProj;
    GLint locScreen;
    GLint locMainTex;
    std::map<std::string, GLUniformValue> values;
    std::map<std::string, GLint> locs;

    GLShader() : program(0), locProj(-1), locScreen(-1), locMainTex(-1) {}

    const UniformInfo* find(const std::string& name) const {
        for (size_t i = 0; i < uniforms.size(); ++i)
            if (uniforms[i].name == name) return &uniforms[i];
        return NULL;
    }

    virtual void setUniform(int index, const float* v, int count) {
        GLUniformValue& u = values[uniforms[index].name];
        u.data.assign(v, v + count);
        u.dirty = true;
    }
    virtual void setUniformTexture(int index, Texture* tex) {
        (void)index;
        (void)tex;
    }

    void apply() {
        for (std::map<std::string, GLUniformValue>::iterator it = values.begin(); it != values.end(); ++it) {
            if (!it->second.dirty) continue;
            it->second.dirty = false;
            const UniformInfo* ui = find(it->first);
            if (!ui) continue;
            std::map<std::string, GLint>::iterator li = locs.find(it->first);
            GLint loc;
            if (li == locs.end()) {
                loc = glGetUniformLocation(program, it->first.c_str());
                locs[it->first] = loc;
            } else {
                loc = li->second;
            }
            if (loc < 0) continue;
            const std::vector<float>& d = it->second.data;
            int comps = ui->components;
            if (ui->type == UNIFORM_MAT) {
                int n = (int)d.size() / (comps * comps);
                if (n < 1) continue;
                if (comps == 2) glUniformMatrix2fv(loc, n, GL_FALSE, &d[0]);
                else if (comps == 3) glUniformMatrix3fv(loc, n, GL_FALSE, &d[0]);
                else glUniformMatrix4fv(loc, n, GL_FALSE, &d[0]);
                continue;
            }
            int n = (int)d.size() / comps;
            if (n < 1) continue;
            if (ui->type == UNIFORM_FLOAT) {
                switch (comps) {
                case 1: glUniform1fv(loc, n, &d[0]); break;
                case 2: glUniform2fv(loc, n, &d[0]); break;
                case 3: glUniform3fv(loc, n, &d[0]); break;
                default: glUniform4fv(loc, n, &d[0]); break;
                }
            } else if (ui->type == UNIFORM_INT || ui->type == UNIFORM_BOOL) {
                std::vector<GLint> iv(d.size());
                for (size_t k = 0; k < d.size(); ++k) iv[k] = (GLint)d[k];
                switch (comps) {
                case 1: glUniform1iv(loc, n, &iv[0]); break;
                case 2: glUniform2iv(loc, n, &iv[0]); break;
                case 3: glUniform3iv(loc, n, &iv[0]); break;
                default: glUniform4iv(loc, n, &iv[0]); break;
                }
            }
        }
    }

    void markAllDirty() {
        for (std::map<std::string, GLUniformValue>::iterator it = values.begin(); it != values.end(); ++it)
            it->second.dirty = true;
    }
};

GLuint compileStage(GLenum type, const std::string& src, std::string& err) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        err += (type == GL_VERTEX_SHADER) ? "vertex: " : "pixel: ";
        err += log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

class GLDevice : public Device {
public:
    GLDevice() : ctx(0), w(0), h(0), backbuffer(NULL), current(NULL), defaultShader(NULL),
                 boundShader(NULL), vbo(0) {}

    virtual bool init(int width, int height) {
        w = width;
        h = height;
        ctx = OSMesaCreateContextExt(OSMESA_RGBA, 0, 0, 0, NULL);
        if (!ctx) return false;
        osBuffer.resize((size_t)w * h * 4);
        if (!OSMesaMakeCurrent(ctx, &osBuffer[0], GL_UNSIGNED_BYTE, w, h)) return false;
        plat::log("[gl] %s / %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));

        backbuffer = (GLTexture*)createRenderTarget(w, h);
        setRenderTarget(NULL);

        std::string err;
        defaultShader = (GLShader*)createShader("", err);
        if (!defaultShader) {
            plat::log("[gl] default shader failed: %s", err.c_str());
            return false;
        }
        glGenBuffers(1, &vbo);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        return true;
    }

    virtual int width() const { return w; }
    virtual int height() const { return h; }

    virtual Texture* createTexture(int tw, int th, const u8* pixels, bool mipmaps) {
        GLTexture* t = new GLTexture;
        t->width = tw;
        t->height = th;
        t->mipmapped = mipmaps;
        glGenTextures(1, &t->id);
        glBindTexture(GL_TEXTURE_2D, t->id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        if (mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
        applyParams(t);
        return t;
    }

    virtual Texture* createRenderTarget(int tw, int th) {
        GLTexture* t = (GLTexture*)createTexture(tw, th, NULL, false);
        t->renderTarget = true;
        glGenFramebuffers(1, &t->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        bindTarget();
        return t;
    }

    virtual void updateTexture(Texture* tex, int x, int y, int tw, int th, const u8* pixels) {
        GLTexture* t = (GLTexture*)tex;
        glBindTexture(GL_TEXTURE_2D, t->id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        if (t->mipmapped) glGenerateMipmap(GL_TEXTURE_2D);
    }

    virtual void destroyTexture(Texture* tex) {
        GLTexture* t = (GLTexture*)tex;
        if (!t) return;
        if (current == t) setRenderTarget(NULL);
        if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
        glDeleteTextures(1, &t->id);
        delete t;
    }

    virtual void setTextureParams(Texture* tex, Filter minF, Filter magF, Wrap ws, Wrap wt) {
        tex->minFilter = minF;
        tex->magFilter = magF;
        tex->wrapS = ws;
        tex->wrapT = wt;
        applyParams((GLTexture*)tex);
    }

    virtual Shader* createShader(const std::string& source, std::string& err) {
        LoveShaderInfo info;
        analyzeLoveShader(source, info);
        GLuint vs = compileStage(GL_VERTEX_SHADER, buildGLSLStage(source, info, true), err);
        GLuint ps = compileStage(GL_FRAGMENT_SHADER, buildGLSLStage(source, info, false), err);
        if (!vs || !ps) {
            if (vs) glDeleteShader(vs);
            if (ps) glDeleteShader(ps);
            return NULL;
        }
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, ps);
        glBindAttribLocation(prog, 0, "VertexPosition");
        glBindAttribLocation(prog, 1, "VertexTexCoord");
        glBindAttribLocation(prog, 2, "VertexColor");
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(ps);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetProgramInfoLog(prog, sizeof(log), NULL, log);
            err += "link: ";
            err += log;
            glDeleteProgram(prog);
            return NULL;
        }
        GLShader* s = new GLShader;
        s->program = prog;
        s->uniforms = info.uniforms;
        s->locProj = glGetUniformLocation(prog, "TransformProjectionMatrix");
        s->locScreen = glGetUniformLocation(prog, "love_ScreenSize");
        s->locMainTex = glGetUniformLocation(prog, "MainTex");
        return s;
    }

    virtual void destroyShader(Shader* sh) {
        GLShader* s = (GLShader*)sh;
        if (!s) return;
        if (boundShader == s) boundShader = NULL;
        glDeleteProgram(s->program);
        delete s;
    }

    virtual void setRenderTarget(Texture* rt) {
        current = rt ? (GLTexture*)rt : backbuffer;
        bindTarget();
    }

    virtual void clear(float r, float g, float b, float a) {
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    virtual void setScissor(bool enabled, int x, int y, int sw, int sh) {
        if (!enabled) {
            glDisable(GL_SCISSOR_TEST);
            return;
        }
        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, sw, sh);   // our y-down convention matches row order
    }

    virtual void setColorMask(bool r, bool g, bool b, bool a) { glColorMask(r, g, b, a); }

    virtual void drawTriangles(const Vertex* verts, int count, Texture* tex, Shader* shader, BlendMode blend) {
        if (count <= 0) return;
        GLShader* s = shader ? (GLShader*)shader : defaultShader;
        if (boundShader != s) {
            glUseProgram(s->program);
            boundShader = s;
        }
        int tw = current->width, th = current->height;
        // Pixel space (y down) -> clip space where y=0 maps to NDC -1 so
        // that row 0 of the target is the top of the image.
        float proj[16] = {
            2.0f / tw, 0, 0, 0,
            0, 2.0f / th, 0, 0,
            0, 0, -1, 0,
            -1, -1, 0, 1};
        if (s->locProj >= 0) glUniformMatrix4fv(s->locProj, 1, GL_FALSE, proj);
        if (s->locScreen >= 0) glUniform4f(s->locScreen, (float)tw, (float)th, 1.0f, 0.0f);
        if (s->locMainTex >= 0) glUniform1i(s->locMainTex, 0);
        s->apply();

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ((GLTexture*)tex)->id);
        applyBlend(blend);

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, count * sizeof(Vertex), verts, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(2 * sizeof(float)));
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(4 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, count);
    }

    virtual void present() { glFlush(); }

    virtual bool readBackbuffer(std::vector<u8>& rgba) {
        GLTexture* prev = current;
        current = backbuffer;
        bindTarget();
        rgba.resize((size_t)w * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[0]);
        current = prev;
        bindTarget();
        return true;
    }

    virtual const char* rendererName() const { return "OpenGL (OSMesa)"; }

private:
    OSMesaContext ctx;
    int w, h;
    std::vector<u8> osBuffer;
    GLTexture* backbuffer;
    GLTexture* current;
    GLShader* defaultShader;
    GLShader* boundShader;
    GLuint vbo;

    void bindTarget() {
        if (!current) return;
        glBindFramebuffer(GL_FRAMEBUFFER, current->fbo);
        glViewport(0, 0, current->width, current->height);
    }

    void applyParams(GLTexture* t) {
        glBindTexture(GL_TEXTURE_2D, t->id);
        GLenum minF = glFilter(t->minFilter);
        if (t->mipmapped) minF = (t->minFilter == FILTER_NEAREST) ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minF);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glFilter(t->magFilter));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrap(t->wrapS));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrap(t->wrapT));
    }

    void applyBlend(BlendMode b) {
        glEnable(GL_BLEND);
        switch (b) {
        case BLEND_ALPHA:
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case BLEND_ALPHA_PREMUL:
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case BLEND_ADD:
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
            break;
        case BLEND_SUBTRACT:
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
            break;
        case BLEND_MULTIPLY:
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_DST_COLOR, GL_ZERO);
            break;
        case BLEND_SCREEN:
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_COLOR);
            break;
        case BLEND_REPLACE:
        default:
            glDisable(GL_BLEND);
            break;
        }
    }
};

}  // namespace

Device* createDevice() { return new GLDevice; }

}  // namespace gfx
