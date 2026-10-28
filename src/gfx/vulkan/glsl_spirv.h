// GLSL to SPIR-V through glslang, as `glslang -V` compiles it (Vulkan 1.0,
// SPIR-V 1.0, no optimiser). On the console this is glslang-xenon, so the
// game's shaders are still translated and compiled at load time.
#ifndef B360_GFX_GLSL_SPIRV_H
#define B360_GFX_GLSL_SPIRV_H

#include "../../common/types.h"
#include <string>
#include <vector>

namespace gfx {

// Returns false with glslang's messages in log on failure.
bool glslToSpirv(bool vertexStage, const std::string& source, std::vector<u32>& spirv, std::string& log);

}  // namespace gfx

#endif
