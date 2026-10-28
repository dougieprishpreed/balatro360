#!/usr/bin/env python3
"""Check that LÖVE shaders build for the console, as the xSDK build compiles them at load time.

Each shader goes through b360-shadertool (the runtime's translator to Vulkan GLSL), glslang -V, then xost, the
compiler vulkan4xenon runs on the console for SPIR-V (xsdk_xsc_compile), with the same register split and the
optimiser on. Run it on your own game's shaders, never commit them:

    tools/xsdk/check_shaders.py --xsdk $XSDK --glslang $XSDK_GLSLANG --tool out/host/b360-shadertool \
        out/Balatro360/game/resources/shaders/*.fs

Exits 1 if any shader fails. --keep DIR keeps the GLSL and SPIR-V there.
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
    print(f"{failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
