#!/usr/bin/env python3
"""Check that LÖVE shaders build for the console, as the xSDK build compiles them at load time.

Each shader goes through b360-shadertool (the runtime's translator to Vulkan GLSL), glslang -V, then xost, the
compiler vulkan4xenon runs on the console for SPIR-V (xsdk_xsc_compile), with the same register split and the
optimiser on. Run it on your own game's shaders, never commit them:

    tools/xsdk/check_shaders.py --xsdk $XSDK --glslang $XSDK_GLSLANG --tool out/host/b360-shadertool \
        out/Balatro360/game/resources/shaders/*.fs

Exits 1 if any shader fails. --keep DIR keeps the GLSL and SPIR-V there. --simulate N also runs each stage on
xost's simulator for N random inputs, which finds some of xost's miscompiles (see simulate_stage).
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--xsdk", required=True, help="xenosworks/xsdk checkout, with build/host/libxsc.so built")
    parser.add_argument("--glslang", required=True, help="host glslang executable")
    parser.add_argument("--tool", required=True, help="host b360-shadertool")
    parser.add_argument("--keep", help="folder to keep the generated GLSL and SPIR-V in")
    parser.add_argument("--empty", action="store_true", help="also check the default shader")
    parser.add_argument("--simulate", type=int, metavar="N", default=0,
                        help="also run N random vertices or pixels through xost's simulator (xdis/simulate.py), "
                             "built with and without its optimiser, and check the stock position()'s M * v")
    parser.add_argument("shaders", nargs="*", type=Path)
    args = parser.parse_args()

    sys.path.insert(0, str(Path(args.xsdk) / "xenos-toolchain"))
    from xasm.program import XSDK_REGISTERS
    from xsc import native

    work = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="b360-shaders-"))
    work.mkdir(parents=True, exist_ok=True)
    shaders = list(args.shaders)
    if args.empty or not shaders:
        default = work / "default.fs"
        default.write_text("")
        shaders.insert(0, default)

    failed = 0
    for shader in shaders:
        for mode, stage in (("vk-vs", "vert"), ("vk-ps", "frag")):
            name = f"{shader.stem}.{stage}"
            glsl = work / name
            spv = work / (name + ".spv")
            result = subprocess.run([args.tool, mode, str(shader)], capture_output=True, text=True)
            glsl.write_text(result.stdout)
            for line in result.stderr.splitlines():
                print(f"{name}: {line}")
            result = subprocess.run([args.glslang, "-V", str(glsl), "-o", str(spv)], capture_output=True, text=True)
            if result.returncode != 0:
                print(f"FAIL {name}: glslang:\n{result.stdout}{result.stderr}")
                failed += 1
                continue
            try:
                container, manifest, resources = native.compile_spirv(
                    spv.read_bytes(), XSDK_REGISTERS.pixel, XSDK_REGISTERS.vertex, True)
            except native.CompileError as error:
                print(f"FAIL {name}: xost: {error}")
                failed += 1
                continue
            print(f"ok   {name}: {len(container)} bytes, {len(manifest)} literals/loops, {len(resources)} resources")
            if args.simulate:
                stock = stage == "vert" and "vec4 position" not in shader.read_text()
                problems = simulate_stage(native, XSDK_REGISTERS, spv.read_bytes(), glsl.read_text(), stage,
                                          args.simulate, stock)
                for line in problems:
                    print(f"FAIL {name}: {line}")
                failed += bool(problems)
    print(f"{failed} failed")
    return 1 if failed else 0


W, H = 1280.0, 720.0


def simulate_stage(native, registers, spv, glsl, stage, count, stock_position):
    """What's wrong with a stage on xost's simulator: optimised and plain builds that disagree for random inputs
    (xost's optimiser has dropped a second length() before), and, for the stock position(), oPos other than M * v
    (xost has multiplied by a matrix parameter's transpose). The block holds random floats past the built-ins and
    each loop's count from the translator's comment; textures are a smooth pattern of their coordinates."""
    import math
    import random
    import re
    import struct

    from xdis import simulate

    rng = random.Random(1)
    block = bytearray(4096)
    struct.pack_into("<20f", block, 0, 2 / W, 0, 0, 0, 0, 2 / H, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1, W, H, 1, 0)
    for m in re.finditer(r"layout\(offset = (\d+)\) (\w+) (\w+)(\[(\d+)\])?;", glsl):
        offset, kind, name = int(m.group(1)), m.group(2), m.group(3)
        if offset < 80:
            continue
        if kind == "int":
            passes = re.search(r"// %s: (\d+) passes" % name, glsl)
            struct.pack_into("<i", block, offset, int(passes.group(1)) if passes else 0)
            continue
        floats = {"float": 1, "vec2": 2, "vec3": 3, "vec4": 4, "mat2": 8, "mat3": 12, "mat4": 16}.get(kind, 4)
        for i in range(floats * int(m.group(5) or 1)):
            struct.pack_into("<f", block, offset + 4 * i, 0.0 if stock_position else rng.uniform(0.0, 1.0))

    def build(optimize):
        container, manifest, resources = native.compile_spirv(spv, registers.pixel, registers.vertex, optimize)
        consts = {m[0]: list(m[1]) for m in manifest if isinstance(m, tuple) and isinstance(m[0], int)}
        loops = {}
        for m in manifest:
            if isinstance(m, tuple) and m and m[0] == "block_loop":
                # The block holds each loop's passes here, which the simulator takes as its loop constant.
                loops[m[1]] = max(0, struct.unpack_from("<i", block, m[5])[0])
            elif isinstance(m, tuple) and m and m[0] == "fixed_loop":
                loops[m[1]] = m[2]
        for kind, _set, _binding, reg, regs in resources:
            if kind == "uniform":
                for k in range(regs):
                    consts[reg + k] = list(struct.unpack_from("<4f", block, 16 * k))
        consts[native.FRAG_COORD_CONSTANT] = [0.5, 0.5, 0.0, 0.0]
        return container, consts, loops

    def texture(slot, coords):
        u, v = coords[0], coords[1]
        return [0.5 + 0.5 * math.sin(7 * u), 0.5 + 0.5 * math.cos(5 * v), (3 * u + 2 * v) % 1.0, 0.75]

    def differ(a, b):
        return max(abs(p - q) if math.isfinite(p) and math.isfinite(q) else float(repr(p) != repr(q))
                   for p, q in zip(a, b))

    builds = (build(True), build(False))
    problems, mismatches, worst = [], 0, 0.0
    for _ in range(count):
        if stage == "vert":
            x, y = rng.uniform(-W, 2 * W), rng.uniform(-H, 2 * H)
            # xost fetches the texcoord, the colour, then the position (locations 2, 1 and 0).
            inputs = {0: [rng.uniform(0, 1), rng.uniform(0, 1), 0.0, 1.0], 1: [rng.uniform(0, 1) for _ in range(4)],
                      2: [x, y, 0.0, 1.0]}
            runs = [simulate.run(c, registers={0: [0.0] * 4}, constants=k, loops=lp,
                                 vertex_fetch=lambda n, fetch, index: inputs[n], texture_fetch=texture)
                    for c, k, lp in builds]
            outputs = [r.exports.get(62, [0.0] * 4) for r in runs]
            if stock_position:
                expected = [2 * x / W - 1, 2 * y / H - 1, 0.0, 1.0]
                if differ(outputs[0], expected) > 1e-4 * max(1.0, abs(x), abs(y)):
                    problems.append(f"position {[round(v, 4) for v in outputs[0]]} for ({x:.0f}, {y:.0f}), "
                                    f"M * v is {[round(v, 4) for v in expected]}")
                    break
        else:
            pixel = {0: [rng.uniform(0, 1), rng.uniform(0, 1), 0.0, 1.0], 1: [rng.uniform(0.5, 1) for _ in range(4)],
                     2: [rng.uniform(0, W), rng.uniform(0, H), 0.0, 0.0]}
            runs = [simulate.run(c, registers=pixel, constants=k, loops=lp, texture_fetch=texture) for c, k, lp in builds]
            outputs = [r.exports.get(0, [0.0] * 4) + [float(r.killed)] for r in runs]
        d = differ(*outputs)
        worst = max(worst, d)
        mismatches += d > 1.0 / 255
    if mismatches > count // 100:
        problems.append(f"optimised and plain builds differ for {mismatches} of {count} inputs (by up to {worst:.3g})")
    return problems


if __name__ == "__main__":
    sys.exit(main())
