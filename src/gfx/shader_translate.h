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

}  // namespace gfx

#endif
