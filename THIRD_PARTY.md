# Third-party code

| Component | Location | License |
|---|---|---|
| Lua 5.1.5 | `third_party/lua` | MIT (see `third_party/lua/COPYRIGHT`) |
| LuaBitOp 1.0.2 | `third_party/luabitop` | MIT (header of `bit.c`) |
| stb_image, stb_image_write, stb_truetype, stb_vorbis | `third_party/stb` | MIT or public domain (see `third_party/stb/LICENSE`) |
| miniz 3.0.2 | `third_party/miniz` | MIT (see `third_party/miniz/LICENSE`) |

The xSDK build also links code fetched at pinned revisions by
`tools/xsdk/bootstrap.sh` (none of it is in this repository); its
`build.sh` copies each licence next to the XEX (`licenses/`):

| Component | Use | License |
|---|---|---|
| [xsdk](https://github.com/xenosworks/xsdk) | runtime, kernel and GPU layer | MIT; parts from libxenon and Xenia, 3-clause BSD (its `THIRD_PARTY_NOTICES.md`) |
| [vulkan4xenon](https://github.com/xenosworks/vulkan4xenon) | Vulkan 1.0 on Xenos | MIT |
| [glslang-xenon](https://github.com/xenosworks/glslang-xenon) | GLSL to SPIR-V on the console | BSD-3-Clause and others (its `LICENSE.txt`) |
| [LuaJIT 2.0](https://luajit.org) | Lua VM | MIT |
| newlib, libgcc, libstdc++ (built by xsdk's toolchain script) | C and C++ runtime | newlib's licences; GPL-3.0 with the GCC Runtime Library Exception |

## Local modifications

* `third_party/lua/src/loadlib.c`: `getenv` is not called on the Xbox 360
  (`_XBOX`), which has no process environment. Marked with a `Balatro360`
  comment.

The runtime's API design follows [LÖVE](https://love2d.org) (zlib license);
no LÖVE source code is included. `src/lua/boot.lua` mirrors the structure of
LÖVE's boot script (event handler table, error handler flow).
