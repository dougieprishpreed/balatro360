// Host tool: translates LÖVE shaders to HLSL / GLSL for inspection and
// offline validation.
//   b360-shadertool hlsl-vs|hlsl-ps|glsl-vs|glsl-ps|vk-vs|vk-ps|vk-ps-noifs|uniforms <file.fs>
// vk-ps-noifs is the Vulkan pixel stage without its uniform ifs (the variant
// drawn while they all fail); it prints nothing for a shader without any.
#include "../gfx/shader_translate.h"

#include <stdio.h>
#include <string.h>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s hlsl-vs|hlsl-ps|glsl-vs|glsl-ps|vk-vs|vk-ps|vk-ps-noifs|uniforms file\n", argv[0]);
        return 2;
    }
    FILE* f = fopen(argv[2], "rb");
    if (!f) {
        perror(argv[2]);
        return 1;
    }
    std::string src;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, n);
    fclose(f);
    gfx::LoveShaderInfo info;
    gfx::analyzeLoveShader(src, info);
    std::string mode = argv[1];
    std::vector<std::string> warnings;
    std::string out;
    if (mode == "hlsl-vs") out = gfx::buildHLSLStage(src, info, true, &warnings);
    else if (mode == "hlsl-ps") out = gfx::buildHLSLStage(src, info, false, &warnings);
    else if (mode == "glsl-vs") out = gfx::buildGLSLStage(src, info, true);
    else if (mode == "glsl-ps") out = gfx::buildGLSLStage(src, info, false);
    else if (mode == "vk-vs") out = gfx::buildVulkanGLSLStage(src, info, true, &warnings);
    else if (mode == "vk-ps") out = gfx::buildVulkanGLSLStage(src, info, false, &warnings);
    else if (mode == "vk-ps-noifs") {
        std::vector<std::vector<gfx::VkLoopGuard> > ifs;
        out = gfx::buildVulkanGLSLStage(src, info, false, &warnings, NULL, &ifs, true);
        if (ifs.empty()) out.clear();
    } else if (mode == "uniforms") {
        for (size_t i = 0; i < info.uniforms.size(); ++i)
            printf("%s type=%d comps=%d array=%d\n", info.uniforms[i].name.c_str(), (int)info.uniforms[i].type,
                   info.uniforms[i].components, info.uniforms[i].arraySize);
        return 0;
    }
    fputs(out.c_str(), stdout);
    for (size_t i = 0; i < warnings.size(); ++i) fprintf(stderr, "warning: %s\n", warnings[i].c_str());
    return 0;
}
