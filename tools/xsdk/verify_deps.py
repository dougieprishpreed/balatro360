#!/usr/bin/env python3
"""Check that the dependency checkouts are at the revisions tools/xsdk/bootstrap.sh pins.

    verify_deps.py xsdk=$XSDK vulkan4xenon=$VULKAN4XENON glslang-xenon=$GLSLANG_XENON luajit=$LUAJIT

A checkout at another revision (or with local changes) fails the build, so a
binary always comes from known sources. B360_ANY_DEPS=1 lets it through with
a warning, for trying newer heads.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

PIN = re.compile(r"^fetch\s+(\S+)\s+\S+\s+\S+\s+([0-9a-f]{40})\s*$")


def pins():
    result = {}
    for line in (Path(__file__).parent / "bootstrap.sh").read_text().splitlines():
        match = PIN.match(line.strip())
        if match:
            result[match.group(1)] = match.group(2)
    return result


def git(path, *args):
    return subprocess.run(["git", "-C", path, *args], capture_output=True, text=True).stdout.strip()


def main():
    wanted = pins()
    problems = []
    for arg in sys.argv[1:]:
        name, _, path = arg.partition("=")
        if name not in wanted:
            sys.exit(f"verify_deps: no pin for {name}")
        head = git(path, "rev-parse", "HEAD")
        dirty = git(path, "status", "--porcelain", "--untracked-files=no")
        if head != wanted[name]:
            problems.append(f"{name} is at {head[:12] or 'nothing'}, the pin is {wanted[name][:12]} ({path})")
        elif dirty:
            problems.append(f"{name} has local changes ({path})")
        else:
            print(f"verify_deps: {name} {head[:12]}")
    if problems:
        for problem in problems:
            print(f"verify_deps: {problem}", file=sys.stderr)
        if os.environ.get("B360_ANY_DEPS") == "1":
            print("verify_deps: B360_ANY_DEPS=1, going on", file=sys.stderr)
            return 0
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
