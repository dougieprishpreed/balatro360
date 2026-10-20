# Host (Linux) build of the Balatro360 runtime, used for development and
# automated testing. The Xbox 360 build uses build/xbox360/*.vcxproj instead.
#
#   make                 offscreen OpenGL build (needs libosmesa6-dev)  -> out/host/b360
#   make GFX=null        no rendering                                   -> out/host-null/b360
#   make ppc             big-endian PowerPC build, run under qemu-ppc   -> out/ppc/b360
#   make LUA=luajit      link the system LuaJIT instead of PUC Lua 5.1  -> out/host-luajit/b360
#
# The flags deliberately restrict the code to C++98 so that it also builds
# with the Visual Studio 2010-era Xbox 360 compiler.

GFX ?= gl
LUA ?= puc
CROSS ?=
OUT ?= out/host$(if $(filter null,$(GFX)),-null,)$(if $(filter luajit,$(LUA)),-luajit,)

CXX := $(CROSS)g++
CC := $(CROSS)gcc

ifeq ($(LUA),luajit)
# System LuaJIT 2.1 (apt install libluajit-5.1-dev); the 360 uses LuaJIT's
# xedkbuild.bat instead, see docs/BUILDING_XDK.md.
LUA_INC := -I/usr/include/luajit-2.1 -DB360_LUAJIT
LUA_LIB := -lluajit-5.1
else
LUA_INC := -Ithird_party/lua/src -Ithird_party/luabitop
LUA_LIB :=
endif
INCLUDES := $(LUA_INC) -Ithird_party/stb -Ithird_party/miniz -Isrc
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
OPT ?= -O2 -g
CXXFLAGS := $(OPT) -std=c++98 -fsigned-char $(WARN) $(INCLUDES)
CFLAGS := $(OPT) -std=c99 -fsigned-char $(INCLUDES)

LUA_SRC := $(filter-out %/lua.c %/luac.c %/print.c %/linit.c %/loslib.c %/liolib.c,$(wildcard third_party/lua/src/*.c))
MINIZ_SRC := third_party/miniz/miniz.c third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c

RUNTIME_SRC := $(wildcard src/runtime/*.cpp) src/gfx/shader_translate.cpp src/third_party_impl.cpp \
	src/platform/posix/platform_posix.cpp src/platform/posix/main_posix.cpp

ifeq ($(GFX),gl)
RUNTIME_SRC += src/gfx/gl/device_gl.cpp
LIBS := -lOSMesa -lpthread -lm
else
RUNTIME_SRC += src/gfx/null/device_null.cpp
LIBS := -lpthread -lm
endif

OBJS := $(patsubst %.c,$(OUT)/%.o,$(MINIZ_SRC)) \
	$(patsubst %.cpp,$(OUT)/%.o,$(RUNTIME_SRC))
ifneq ($(LUA),luajit)
OBJS += $(patsubst %.c,$(OUT)/%.o,$(LUA_SRC)) $(OUT)/third_party/luabitop/bit.o
endif

all: $(OUT)/b360 $(OUT)/b360-shadertool

$(OUT)/b360: $(OBJS)
	$(CXX) -o $@ $^ $(LUA_LIB) $(LIBS) $(LDFLAGS)

$(OUT)/b360-shadertool: $(OUT)/src/tools/shadertool.o $(OUT)/src/gfx/shader_translate.o
	$(CXX) -o $@ $^ $(LDFLAGS)

src/runtime/embedded_lua.h: $(wildcard src/lua/*.lua) tools/embed_lua.py
	python3 tools/embed_lua.py

$(OUT)/src/runtime/runtime.o: src/runtime/embedded_lua.h

# Lua (and LuaBitOp) are compiled as C++ so Lua errors unwind C++ frames.
$(OUT)/third_party/lua/%.o: third_party/lua/%.c
	@mkdir -p $(dir $@)
	$(CXX) -x c++ $(filter-out -Wextra -Wall,$(CXXFLAGS)) -c $< -o $@

$(OUT)/third_party/luabitop/bit.o: third_party/luabitop/bit.c
	@mkdir -p $(dir $@)
	$(CXX) -x c++ $(filter-out -Wextra -Wall,$(CXXFLAGS)) -c $< -o $@

$(OUT)/third_party/miniz/%.o: third_party/miniz/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/src/third_party_impl.o: src/third_party_impl.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(filter-out -Wextra -Wall -std=c++98,$(CXXFLAGS)) -std=c++98 -w -c $< -o $@

$(OUT)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

ppc:
	$(MAKE) GFX=null CROSS=powerpc-linux-gnu- OUT=out/ppc LDFLAGS=-static

clean:
	rm -rf out

.PHONY: all ppc clean
