#!/usr/bin/env bash
# Builds default.xex with the open xSDK toolchain (Linux or WSL). The paths
# come from tools/xsdk/bootstrap.sh, which prints them:
#
#   XSDK=... VULKAN4XENON=... GLSLANG_XENON=... LUAJIT=... XSDK_TOOLCHAIN=... tools/xsdk/build.sh
#
# Output: $B360_XSDK_BUILD (default build-xsdk/output)/default.xex, with the
# licences the XEX ships under in licenses/. B360_LUA=puc builds with PUC Lua
# 5.1 instead of LuaJIT.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
: "${XSDK:?Set XSDK to the pinned xenosworks/xsdk checkout}"
: "${VULKAN4XENON:?Set VULKAN4XENON to its pinned checkout}"
: "${GLSLANG_XENON:?Set GLSLANG_XENON to its pinned checkout}"
: "${XSDK_TOOLCHAIN:?Set XSDK_TOOLCHAIN to the compiler installation prefix}"
OUT="${B360_XSDK_BUILD:-$ROOT/build-xsdk/output}"
JOBS="${JOBS:-1}"
LUA="${B360_LUA:-luajit}"
mkdir -p "$OUT"

DEPS=(xsdk="$XSDK" vulkan4xenon="$VULKAN4XENON" glslang-xenon="$GLSLANG_XENON")
if [ "$LUA" = luajit ]; then
  : "${LUAJIT:?Set LUAJIT to the pinned LuaJIT 2.0 checkout}"
  DEPS+=(luajit="$LUAJIT")
fi
python3 "$ROOT/tools/xsdk/verify_deps.py" "${DEPS[@]}"

TC="$XSDK_TOOLCHAIN/bin/powerpc-xsdk-elf-"
make -C "$XSDK" -j"$JOBS" TOOLCHAIN="$TC" build/libxsdk.a build/host/libxsc.so
make -C "$VULKAN4XENON" -j"$JOBS" XSDK="$XSDK" TOOLCHAIN="$TC" build/libvulkan4xenon.a

LUA_OPT=()
if [ "$LUA" = luajit ]; then
  # Rebuilt only when LuaJIT or the script changes.
  stamp="$(git -C "$LUAJIT" rev-parse HEAD) $(sha256sum "$ROOT/tools/xsdk/build_luajit.sh" | cut -d' ' -f1)"
  if [ ! -f "$OUT/luajit/libluajit.a" ] || [ "$(cat "$OUT/luajit/stamp" 2>/dev/null)" != "$stamp" ]; then
    XSDK_TOOLCHAIN="$XSDK_TOOLCHAIN" JOBS="$JOBS" bash "$ROOT/tools/xsdk/build_luajit.sh" "$LUAJIT" "$OUT/luajit"
    echo "$stamp" >"$OUT/luajit/stamp"
  fi
  LUA_OPT=(-DB360_LUAJIT="$OUT/luajit")
fi

# The first line of b360.log names the commit ("+": local changes).
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || true)"
if [ -n "$COMMIT" ] && [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
  COMMIT="$COMMIT+"
fi
DATE="$(git -C "$ROOT" log -1 --format=%cI 2>/dev/null || true)"

cmake -S "$ROOT/build/xsdk" -B "$OUT/cmake" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$XSDK/cmake/xsdk.cmake" -DXSDK_TOOLCHAIN="$XSDK_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
  -DVULKAN4XENON="$VULKAN4XENON" -DGLSLANG_XENON="$GLSLANG_XENON" -DB360_LUAJIT= "${LUA_OPT[@]}" \
  -DB360_GIT_COMMIT="$COMMIT" -DB360_GIT_DATE="$DATE"
cmake --build "$OUT/cmake" -j"$JOBS"

cp "$OUT/cmake/balatro360.xex" "$OUT/default.xex"
mkdir -p "$OUT/licenses"
cp "$XSDK/LICENSE" "$OUT/licenses/xsdk.txt"
cp "$XSDK/THIRD_PARTY_NOTICES.md" "$OUT/licenses/xsdk-third-party.md"
cp "$VULKAN4XENON/LICENSE" "$OUT/licenses/vulkan4xenon.txt"
cp "$GLSLANG_XENON/LICENSE.txt" "$OUT/licenses/glslang.txt"
[ "$LUA" = luajit ] && cp "$LUAJIT/COPYRIGHT" "$OUT/licenses/luajit.txt"
cp "$XSDK_TOOLCHAIN"/share/*-licenses/* "$OUT/licenses/" 2>/dev/null || true
echo "Built $OUT/default.xex ($(stat -c %s "$OUT/default.xex") bytes, source $COMMIT)"
