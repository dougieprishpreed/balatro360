// glslang's C++ API needs C++17, so this file alone is built as C++17; its
// interface (glsl_spirv.h) is plain C++98.
#include "glsl_spirv.h"

#include "SPIRV/GlslangToSpv.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"

namespace gfx {

bool glslToSpirv(bool vertexStage, const std::string& source, std::vector<u32>& spirv, std::string& log) {
    static bool initialised = false;
    if (!initialised) {
        glslang::InitializeProcess();
        initialised = true;
    }
    const EShLanguage stage = vertexStage ? EShLangVertex : EShLangFragment;
    const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    const char* text = source.c_str();
    glslang::TShader shader(stage);
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        log = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        log = program.getInfoLog();
        return false;
    }
    std::vector<unsigned int> words;
    glslang::SpvOptions options;
    glslang::GlslangToSpv(*program.getIntermediate(stage), words, &options);
    spirv.assign(words.begin(), words.end());
    return !spirv.empty();
}

}  // namespace gfx
