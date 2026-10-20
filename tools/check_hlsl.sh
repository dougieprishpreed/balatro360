#!/bin/bash
# Translates every LÖVE shader given on the command line to HLSL and compiles
# it with vkd3d-compiler (1.14+) as vs_3_0 and ps_3_0.
#   tools/check_hlsl.sh /path/to/game/resources/shaders/*.fs
# Set VKD3D_COMPILER to the compiler binary if it isn't on PATH.
cd "$(dirname "$0")/.."
VKD3D=${VKD3D_COMPILER:-vkd3d-compiler}
TOOL=out/host/b360-shadertool
TMP=$(mktemp -d)
fail=0
for f in "$@"; do
    b=$(basename "$f" .fs)
    for st in vs ps; do
        $TOOL hlsl-$st "$f" > "$TMP/$b.$st.hlsl"
        if $VKD3D -x hlsl -b d3dbc -p ${st}_3_0 -e love_main -o /dev/null "$TMP/$b.$st.hlsl" > "$TMP/$b.$st.log" 2>&1; then
            echo "OK    $b.$st"
        elif grep -q "HLSL_IR_LOOP" "$TMP/$b.$st.log"; then
            echo "SKIP  $b.$st (loops unsupported by vkd3d's SM3 backend)"
        else
            echo "FAIL  $b.$st"; grep -v fixme "$TMP/$b.$st.log" | head -5; fail=1
        fi
    done
done
rm -rf "$TMP"
exit $fail
