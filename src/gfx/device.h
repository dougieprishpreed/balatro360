// Rendering backend interface.
//
// The core love.graphics implementation (src/runtime/graphics.cpp) does
// all transforms, batching and state tracking on the CPU and hands the
// backend plain triangle lists. Backends:
//   src/gfx/gl/    OpenGL 2.1 (OSMesa) for the Linux host build
//   src/gfx/d3d9/  Direct3D 9 for the Xbox 360
//
// Coordinate conventions shared by all backends:
//   * Vertex positions are in pixels, origin top-left, y down, already
//     transformed by the love.graphics transform stack.
//   * Texture row 0 is the top of the image, (u,v) = (0,0) is top-left.
//   * Render targets use the same orientation, so a canvas can be drawn
//     like any image without flipping.
#ifndef B360_GFX_DEVICE_H
#define B360_GFX_DEVICE_H

#include "../common/types.h"
#include <string>
#include <vector>

namespace gfx {

struct Vertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};

enum Filter { FILTER_NEAREST = 0, FILTER_LINEAR };
enum Wrap { WRAP_CLAMP = 0, WRAP_REPEAT, WRAP_MIRROR };

enum TexFormat {
    TEX_RGBA8 = 0,   // 4 bytes per pixel, R,G,B,A in memory order
};

enum BlendMode {
    BLEND_ALPHA = 0,        // LÖVE "alpha" + "alphamultiply"
    BLEND_ALPHA_PREMUL,     // LÖVE "alpha" + "premultiplied"
    BLEND_ADD,
    BLEND_SUBTRACT,
    BLEND_MULTIPLY,
    BLEND_REPLACE,
    BLEND_SCREEN,
};

enum UniformType {
    UNIFORM_FLOAT = 0,  // float, vec2, vec3, vec4 (components 1..4)
    UNIFORM_INT,        // int / ivecN
    UNIFORM_BOOL,       // bool / bvecN
    UNIFORM_MAT,        // matN (components = N)
    UNIFORM_SAMPLER,    // Image / sampler2D
};

struct UniformInfo {
    std::string name;
    UniformType type;
    int components;   // vector size or matrix dimension
    int arraySize;    // 1 for non-arrays
};

class Texture {
public:
    virtual ~Texture() {}
    int width;    // pixels
    int height;
    bool renderTarget;
    Filter minFilter, magFilter;
    Wrap wrapS, wrapT;
protected:
    Texture() : width(0), height(0), renderTarget(false),
                minFilter(FILTER_LINEAR), magFilter(FILTER_LINEAR),
                wrapS(WRAP_CLAMP), wrapT(WRAP_CLAMP) {}
};

class Shader {
public:
    virtual ~Shader() {}
    // Uniforms declared by the shader source (parsed by the core).
    std::vector<UniformInfo> uniforms;
    // index refers to uniforms[]. values holds the elements one after the
    // other (components each; components^2 for matrices). The core only
    // calls this when a value actually changed; the backend keeps it and
    // uploads it whenever the shader is used for drawing.
    virtual void setUniform(int index, const float* values, int count) = 0;
    virtual void setUniformTexture(int index, Texture* tex) = 0;
};

class Device {
public:
    virtual ~Device() {}

    virtual bool init(int width, int height) = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;

    // pixels: tightly packed RGBA8, rows top to bottom. May be NULL.
    virtual Texture* createTexture(int w, int h, const u8* pixels, bool mipmaps) = 0;
    virtual Texture* createRenderTarget(int w, int h) = 0;
    virtual void updateTexture(Texture* t, int x, int y, int w, int h, const u8* pixels) = 0;
    virtual void destroyTexture(Texture* t) = 0;
    virtual void setTextureParams(Texture* t, Filter minF, Filter magF, Wrap ws, Wrap wt) = 0;

    // source: LÖVE-dialect GLSL (the contents of a .fs / .glsl file).
    // Returns NULL and fills err on failure.
    virtual Shader* createShader(const std::string& source, std::string& err) = 0;
    virtual void destroyShader(Shader* s) = 0;

    // NULL = back buffer.
    virtual void setRenderTarget(Texture* rt) = 0;
    virtual void clear(float r, float g, float b, float a) = 0;
    virtual void setScissor(bool enabled, int x, int y, int w, int h) = 0;
    virtual void setColorMask(bool r, bool g, bool b, bool a) = 0;

    // Draw a triangle list. tex is never NULL (core binds a white texture).
    // shader may be NULL for the default LÖVE shader.
    virtual void drawTriangles(const Vertex* verts, int count, Texture* tex,
                               Shader* shader, BlendMode blend) = 0;

    virtual void present() = 0;

    // Frame pacing: show each frame for this many vertical blanks (1 = 60 Hz,
    // 2 = 30 Hz). Returns false if the backend can't, in which case the core
    // paces frames itself.
    virtual bool setPresentInterval(int vblanks) { (void)vblanks; return false; }

    // Read back the current back buffer as RGBA8 (for host screenshots).
    virtual bool readBackbuffer(std::vector<u8>& rgba) { (void)rgba; return false; }

    virtual const char* rendererName() const = 0;
};

Device* createDevice();

}  // namespace gfx

#endif
