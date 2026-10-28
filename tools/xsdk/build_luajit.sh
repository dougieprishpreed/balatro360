#!/usr/bin/env bash
# Builds LuaJIT 2.0's PowerPC interpreter for the console with the xSDK toolchain.
#
# This is the VM of LuaJIT's own Xbox 360 build (src/xedkbuild.bat): the same
# DynASM flags and the same C defines, so the interpreter keeps the Xbox 360
# calling convention, which xSDK's GCC implements (-mcall-xsdk). The only
# difference is that buildvm writes ELF assembly instead of a PE object.
#
#   tools/xsdk/build_luajit.sh <LuaJIT 2.0 checkout> <output folder>
#
# Leaves <output folder>/libluajit.a and <output folder>/include/*.h.
set -euo pipefail
: "${XSDK_TOOLCHAIN:?Set XSDK_TOOLCHAIN to the compiler installation prefix}"
SRC="$(cd "${1:?LuaJIT 2.0 checkout}" && pwd)"
OUT="$(mkdir -p "${2:?output folder}" && cd "$2" && pwd)"
JOBS="${JOBS:-1}"
HOST_CC="${HOST_CC:-gcc -m32}"
CC="$XSDK_TOOLCHAIN/bin/powerpc-xsdk-elf-gcc"
AR="$XSDK_TOOLCHAIN/bin/powerpc-xsdk-elf-ar"

if ! grep -q 'LUAJIT_VERSION_NUM.*200' "$SRC/src/luajit_rolling.h" 2>/dev/null; then
  echo "error: $SRC is not LuaJIT 2.0 (2.1 has no Xbox 360 VM)" >&2
  exit 1
fi

# Work on a copy, so the checkout stays clean and a rebuild starts fresh.
WORK="$OUT/work"
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$SRC/src" "$SRC/dynasm" "$WORK/"
# The release number is the commit's time: .relver in an archive, git in a clone.
if [ -d "$SRC/.git" ]; then
  git -C "$SRC" show -s --format=%ct >"$WORK/src/luajit_relver.txt"
else
  cp "$SRC/.relver" "$WORK/src/luajit_relver.txt"
fi
cd "$WORK/src"

# Target OS "Other" (no Linux headers), Xbox 360 target, no external unwinding:
# Lua errors unwind with LuaJIT's own unwinder, as with the XDK.
DEFS=(-D_XBOX_VER=200 -DLUAJIT_TARGET=LUAJIT_ARCH_PPC -DLUAJIT_OS=LUAJIT_OS_OTHER -DLUAJIT_NO_UNWIND)

$HOST_CC -O2 -o minilua host/minilua.c -lm
./minilua ../dynasm/dynasm.lua -LN -D GPR64 -D FRAME32 -D PPE -D SQRT -D DUALNUM -o host/buildvm_arch.h vm_ppc.dasc
./minilua host/genversion.lua
$HOST_CC -O2 -I. -I../dynasm "${DEFS[@]}" -o buildvm host/buildvm*.c

LIBS=(lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c)
./buildvm -m elfasm -o lj_vm.s
./buildvm -m bcdef -o lj_bcdef.h "${LIBS[@]}"
./buildvm -m ffdef -o lj_ffdef.h "${LIBS[@]}"
./buildvm -m libdef -o lj_libdef.h "${LIBS[@]}"
./buildvm -m recdef -o lj_recdef.h "${LIBS[@]}"
./buildvm -m folddef -o lj_folddef.h lj_opt_fold.c

# -ffp-contract=off is /fp:strict: fused multiply-adds change math.random's
# results, and seeds must match the PC (tests/rng).
CFLAGS=(-mcpu=cell -fno-pic -O2 -ffp-contract=off -fomit-frame-pointer -DNDEBUG -DLUAJIT_USE_SYSMALLOC "${DEFS[@]}")
"$CC" -mcpu=cell -c lj_vm.s -o lj_vm.o
printf '%s\n' lj_*.c lib_*.c | xargs -P "$JOBS" -I{} "$CC" "${CFLAGS[@]}" -c {}
rm -f "$OUT/libluajit.a"
"$AR" rcs "$OUT/libluajit.a" lj_*.o lib_*.o

mkdir -p "$OUT/include"
cp lua.h lualib.h lauxlib.h luaconf.h lua.hpp luajit.h "$OUT/include/"
echo "LuaJIT for xSDK: $OUT/libluajit.a"
