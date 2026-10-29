// LÖVE shader front end.
//
// LÖVE shaders are written in a GLSL dialect ("extern", "number", "Image",
// "Texel", effect()/position() entry points). This module
//   * analyses a LÖVE shader (which stages it has, which uniforms it uses),
//   * wraps it into complete GLSL 1.20 stages for the OpenGL backend, and
//   * translates it into HLSL (shader model 3) for the Direct3D 9 / Xbox 360
//     backend.
// The translator is token based and covers the subset of GLSL used by
// Balatro's shaders (and typical LÖVE 2D shaders).
#ifndef B360_SHADER_TRANSLATE_H
#define B360_SHADER_TRANSLATE_H

#include "device.h"
#include <string>
#include <vector>

namespace gfx {

struct LoveShaderInfo {
    bool hasPosition;   // defines vec4 position(mat4, vec4)
    bool hasEffect;     // defines vec4 effect(vec4, Image, vec2, vec2)
    std::vector<UniformInfo> uniforms;
};

void analyzeLoveShader(const std::string& src, LoveShaderInfo& info);

// Complete GLSL 1.20 source for one stage.
std::string buildGLSLStage(const std::string& src, const LoveShaderInfo& info, bool vertexStage);

// Complete HLSL source for one stage. Entry point: "love_main".
// Built-in uniforms expected by the generated code:
//   float4 love_ProjRows[4]  rows of the projection matrix (pixels -> clip)
//   float4 love_ScreenSize   (w, h, 1, 0)
//   sampler2D love_MainTex   (s0)
// Vertex input: float2 POSITION, float2 TEXCOORD0, float4 COLOR0.
std::string buildHLSLStage(const std::string& src, const LoveShaderInfo& info, bool vertexStage,
                           std::vector<std::string>* warnings);

// A pixel-stage loop the Vulkan translator made a hardware loop: its
// condition became "i < b360_loopK", an int in LoveUniforms that the backend
// sets to count, or to start (no passes) while a guard fails. The guards are
// the uniform comparisons of the ifs around it, which xost doesn't branch on
// when an arm holds a loop: it runs both arms for every pixel.
struct VkLoopGuard {
    enum Op { GT, GE, LT, LE };
    int uniform;   // index into LoveShaderInfo::uniforms, a float
    Op op;
    float value;   // uniforms[uniform] op value
};
struct VkHardwareLoop {
    int start, count;
    std::vector<VkLoopGuard> guards;   // all must hold
};

// Complete Vulkan GLSL 4.50 source for one stage, for glslang -V (Vulkan
// backend). Interface:
//   set 0, binding 0: std140 uniform block LoveUniforms, at a dynamic offset:
//                     mat4 TransformProjectionMatrix, vec4 love_ScreenSize,
//                     vec4 b360_FragCoordMap (screen_coords = gl_FragCoord.xy
//                     * .xy + .zw, normally (1, 1, 0, 0)), then the shader's
//                     uniforms (vulkanUniformLayout), then the pixel stage's
//                     loop counts, an int each
//   set 0, binding 1: sampler2D love_MainTex
//   vertex inputs:    location 0 vec2 position, 1 vec4 colour, 2 vec2 texcoord
// Functions with early returns are rewritten to return once, as xost (the
// console's shader compiler) inlines only those. loops (pixel stage) gets
// the hardware loops, in the order of their ints.
//
// A pixel stage's ifs whose condition only compares uniforms with numbers
// (CRT's bloom and glitch) cost every pixel on the console, which runs or
// skips each arm's instructions but keeps the code around it. ifs, when not
// NULL, gets each such if's guards (all of an if's must hold for it to run);
// withoutIfs leaves those ifs out, for a variant drawn while every one of
// them fails. The variant keeps the block's layout and loop ints.
std::string buildVulkanGLSLStage(const std::string& src, const LoveShaderInfo& info, bool vertexStage,
                                 std::vector<std::string>* warnings, std::vector<VkHardwareLoop>* loops = NULL,
                                 std::vector<std::vector<VkLoopGuard> >* ifs = NULL, bool withoutIfs = false);

// std140 byte offset of each of info.uniforms in LoveUniforms (-1 for
// samplers), and the block's size. bools and ints are stored as floats.
// The loop counts follow at blockBytes, 4 bytes apart (vulkanLoopBlockBytes
// is the size with them).
enum { VK_BUILTIN_UNIFORM_BYTES = 96 };
void vulkanUniformLayout(const LoveShaderInfo& info, std::vector<int>& offsets, int& blockBytes);
int vulkanLoopBlockBytes(int blockBytes, int loops);

}  // namespace gfx

#endif
