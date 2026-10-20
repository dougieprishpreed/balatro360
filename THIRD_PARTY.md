# Third-party code

| Component | Location | License |
|---|---|---|
| Lua 5.1.5 | `third_party/lua` | MIT (see `third_party/lua/COPYRIGHT`) |
| LuaBitOp 1.0.2 | `third_party/luabitop` | MIT (header of `bit.c`) |
| stb_image, stb_image_write, stb_truetype, stb_vorbis | `third_party/stb` | MIT or public domain (see `third_party/stb/LICENSE`) |
| miniz 3.0.2 | `third_party/miniz` | MIT (see `third_party/miniz/LICENSE`) |

## Local modifications

* `third_party/lua/src/loadlib.c`: `getenv` is not called on the Xbox 360
  (`_XBOX`), which has no process environment. Marked with a `Balatro360`
  comment.

The runtime's API design follows [LÖVE](https://love2d.org) (zlib license);
no LÖVE source code is included. `src/lua/boot.lua` mirrors the structure of
LÖVE's boot script (event handler table, error handler flow).
