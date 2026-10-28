#!/usr/bin/env bash
# Fetches the pinned open dependencies and builds the xSDK compiler, its
# libraries and a host glslang (for tools/xsdk/check_shaders.py). Run on
# Linux or in WSL after installing the prerequisites in
# docs/BUILDING_XSDK.md. No Microsoft files are used.
#
# The pins below are the only versions the build accepts (verify_deps.py);
# move them together, and expect a GCC rebuild (about 30 minutes) when
# xsdk's tools/toolchain changes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="${B360_XSDK_DEPS:-$ROOT/build-xsdk/deps}"
PREFIX="${XSDK_TOOLCHAIN:-$ROOT/build-xsdk/toolchain}"
JOBS="${JOBS:-1}"
mkdir -p "$DEPS"

fetch() {
  local name="$1" url="$2" ref="$3" sha="$4"
  if [ ! -d "$DEPS/$name/.git" ]; then
    git clone --depth 1 --branch "$ref" "$url" "$DEPS/$name"
  fi
  if [ "$(git -C "$DEPS/$name" rev-parse HEAD)" != "$sha" ]; then
    git -C "$DEPS/$name" fetch --depth 1 origin "$sha"
    git -C "$DEPS/$name" checkout --detach "$sha"
  fi
}
fetch xsdk https://github.com/xenosworks/xsdk.git main 8c58c7a3fb37bb261c5a11a8a1f4fa8a8bfd0d28
fetch vulkan4xenon https://github.com/xenosworks/vulkan4xenon.git main df25dc50e6296d89822626b433f4080db05bccc8
fetch glslang-xenon https://github.com/xenosworks/glslang-xenon.git xenon d2f00185cf5feaa449f6e15c1894fb232b2b6bcd
fetch luajit https://github.com/LuaJIT/LuaJIT.git v2.0 2410b57fd466298e6a64a68986a6b696c23656ac

(cd "$DEPS/xsdk"; PREFIX="$PREFIX" WORK="$DEPS/toolchain-work" JOBS="$JOBS" bash tools/toolchain/build.sh)
mkdir -p "$PREFIX/share/b360-licenses"
cp "$DEPS"/toolchain-work/src/newlib-*/COPYING.NEWLIB "$PREFIX/share/b360-licenses/newlib.txt"
cp "$DEPS"/toolchain-work/src/gcc-*/COPYING3 "$PREFIX/share/b360-licenses/GPL-3.0.txt"
cp "$DEPS"/toolchain-work/src/gcc-*/COPYING.RUNTIME "$PREFIX/share/b360-licenses/GCC-runtime-exception.txt"

# A host glslang, for checking shaders against xost (the console's compiler).
if [ "${B360_HOST_GLSLANG:-1}" = 1 ]; then
  (cd "$DEPS/glslang-xenon"; python3 update_glslang_sources.py)
  cmake -S "$DEPS/glslang-xenon" -B "$DEPS/glslang-host" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=ON -DBUILD_TESTING=OFF -DGLSLANG_TESTS=OFF
  cmake --build "$DEPS/glslang-host" -j"$JOBS"
fi

printf '\nUse these with tools/xsdk/build.sh:\nXSDK=%s\nVULKAN4XENON=%s\nGLSLANG_XENON=%s\nLUAJIT=%s\nXSDK_TOOLCHAIN=%s\n' \
  "$DEPS/xsdk" "$DEPS/vulkan4xenon" "$DEPS/glslang-xenon" "$DEPS/luajit" "$PREFIX"
