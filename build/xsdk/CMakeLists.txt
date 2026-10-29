# Xbox 360 build on the open xSDK (github.com/xenosworks/xsdk) and
# vulkan4xenon. tools/xsdk/build.sh configures it with xSDK's toolchain file;
# see docs/BUILDING_XSDK.md.
cmake_minimum_required(VERSION 3.22)
project(balatro360 C CXX)

if(NOT XSDK)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake")
endif()

set(VULKAN4XENON "" CACHE PATH "vulkan4xenon checkout, with build/libvulkan4xenon.a built")
set(GLSLANG_XENON "" CACHE PATH "glslang-xenon checkout")
set(B360_LUAJIT "" CACHE PATH "LuaJIT built by tools/xsdk/build_luajit.sh (empty: PUC Lua 5.1)")
set(B360_GIT_COMMIT "" CACHE STRING "Source commit for the first line of b360.log")
set(B360_GIT_DATE "" CACHE STRING "Commit date")
set(B360_TITLE_ID "0x42333630" CACHE STRING "The XEX's title ID ('B360')")

get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# The version, in the XEX too (the Homebrew App Store compares it): major.minor.build.fix.
file(STRINGS "${ROOT}/tools/release/VERSION" B360_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ROOT}/tools/release/VERSION")

# glslang compiles the game's shaders to SPIR-V on the console: GLSL only.
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${GLSLANG_XENON}" glslang EXCLUDE_FROM_ALL)
# Its own warnings aren't ours to fix.
foreach(t glslang MachineIndependent GenericCodeGen OSDependent SPIRV glslang-default-resource-limits)
    if(TARGET ${t})
        target_compile_options(${t} PRIVATE -w)
    endif()
endforeach()

file(GLOB RUNTIME_SOURCES "${ROOT}/src/runtime/*.cpp")
set(SOURCES
    ${RUNTIME_SOURCES}
    "${ROOT}/src/gfx/shader_translate.cpp"
    "${ROOT}/src/gfx/threaded_device.cpp"
    "${ROOT}/src/gfx/vulkan/device_vk.cpp"
    "${ROOT}/src/platform/xsdk/platform_xsdk.cpp"
    "${ROOT}/src/platform/xsdk/main_xsdk.cpp")
set(THIRD_PARTY_SOURCES
    "${ROOT}/src/third_party_impl.cpp"
    "${ROOT}/third_party/miniz/miniz.c"
    "${ROOT}/third_party/miniz/miniz_tdef.c"
    "${ROOT}/third_party/miniz/miniz_tinfl.c")

add_executable(balatro360 ${SOURCES} ${THIRD_PARTY_SOURCES} "${ROOT}/src/gfx/vulkan/glsl_spirv.cpp")
target_include_directories(balatro360 PRIVATE
    "${ROOT}/src" "${ROOT}/third_party/stb" "${ROOT}/third_party/miniz"
    "${VULKAN4XENON}/include" "${VULKAN4XENON}/third_party/vulkan-headers/include")

# char is signed, as with the XDK and the host build; no strict aliasing, as
# with the XDK's compiler; no fused multiply-adds (/fp:strict), which would
# change math.random's results against the PC.
set(COMMON_FLAGS -O2 -g -fsigned-char -fno-strict-aliasing -ffp-contract=off -ffunction-sections -fdata-sections)
set(WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_compile_options(balatro360 PRIVATE ${COMMON_FLAGS})
set_source_files_properties(${SOURCES} PROPERTIES COMPILE_OPTIONS "-std=gnu++98;${WARNINGS}")
set_source_files_properties(${THIRD_PARTY_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
# glslang's C++ API needs C++17; its interface to the rest is C++98.
set_source_files_properties("${ROOT}/src/gfx/vulkan/glsl_spirv.cpp" PROPERTIES COMPILE_OPTIONS "-std=gnu++17;${WARNINGS}")
set_source_files_properties("${ROOT}/third_party/miniz/miniz.c" "${ROOT}/third_party/miniz/miniz_tdef.c"
    "${ROOT}/third_party/miniz/miniz_tinfl.c" PROPERTIES COMPILE_OPTIONS "-std=gnu99;-w")

target_compile_definitions(balatro360 PRIVATE B360_VERSION="${B360_VERSION}")
if(B360_GIT_COMMIT)
    target_compile_definitions(balatro360 PRIVATE B360_GIT_COMMIT="${B360_GIT_COMMIT}" B360_GIT_DATE="${B360_GIT_DATE}")
endif()
# Draw on a render thread, with frame interpolation (src/gfx/threaded_device.h).
target_compile_definitions(balatro360 PRIVATE B360_RENDER_THREAD)

if(B360_LUAJIT)
    target_compile_definitions(balatro360 PRIVATE B360_LUAJIT)
    target_include_directories(balatro360 PRIVATE "${B360_LUAJIT}/include")
    set(LUA_LIBRARY "${B360_LUAJIT}/libluajit.a")
else()
    # PUC Lua 5.1, compiled as C++ so Lua errors are C++ exceptions.
    file(GLOB LUA_SOURCES "${ROOT}/third_party/lua/src/*.c")
    list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|print|linit|loslib|liolib)\\.c$")
    add_library(lua51 STATIC ${LUA_SOURCES} "${ROOT}/third_party/luabitop/bit.c")
    set_source_files_properties(${LUA_SOURCES} "${ROOT}/third_party/luabitop/bit.c" PROPERTIES LANGUAGE CXX)
    target_compile_options(lua51 PRIVATE ${COMMON_FLAGS} -std=gnu++98 -w)
    target_include_directories(lua51 PUBLIC "${ROOT}/third_party/lua/src" "${ROOT}/third_party/luabitop")
    set(LUA_LIBRARY lua51)
endif()

target_link_libraries(balatro360 PRIVATE
    "${VULKAN4XENON}/build/libvulkan4xenon.a" ${LUA_LIBRARY} glslang glslang-default-resource-limits)
set_property(TARGET balatro360 APPEND PROPERTY LINK_DEPENDS "${VULKAN4XENON}/build/libvulkan4xenon.a")
# xSDK's import stubs have no .note.GNU-stack; the console has no such thing.
target_link_options(balatro360 PRIVATE "-Wl,--gc-sections,--no-warn-execstack,-Map,${CMAKE_CURRENT_BINARY_DIR}/balatro360.map")
xsdk_add_xex(balatro360 TITLE_ID ${B360_TITLE_ID} NAME "Balatro360" VERSION ${B360_VERSION})
