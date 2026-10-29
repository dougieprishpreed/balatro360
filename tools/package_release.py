#!/usr/bin/env python3
"""Builds the release zip from the GitHub Actions build. No game data goes in.

    python tools/package_release.py ARTIFACT --version 1.0.0 [--out out/release]

ARTIFACT is the "balatro360-xex" artifact of the xSDK build workflow, as its
zip or the folder it was extracted to (default.xex, default.xex.sha256,
licenses/, THIRD_PARTY.md). The zip holds:

    Balatro360-v<version>/
        Balatro360/           default.xex, THIRD_PARTY.md, licenses/ (copied to the console)
        Convert Balatro.bat   runs converter/convert.ps1: copies the game files
        converter/            convert.ps1, prepare_game.py (Mac/Linux, by hand)
        README.txt
"""
import argparse
import hashlib
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE = os.path.join(ROOT, "tools", "release")


def read_artifact(path):
    """{name: bytes} of the artifact's files, from its zip or folder."""
    files = {}
    if os.path.isdir(path):
        for base, _, names in os.walk(path):
            for name in names:
                full = os.path.join(base, name)
                with open(full, "rb") as f:
                    files[os.path.relpath(full, path).replace(os.sep, "/")] = f.read()
    else:
        with zipfile.ZipFile(path) as zf:
            for info in zf.infolist():
                if not info.is_dir():
                    files[info.filename] = zf.read(info)
    return files


def crlf(data):
    """Text with Windows line endings, for Notepad and cmd.exe."""
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("artifact", help="the balatro360-xex artifact (zip or folder)")
    ap.add_argument("--version", required=True, help="release version, e.g. 1.0.0")
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "release"), help="folder for the zip")
    args = ap.parse_args()

    files = read_artifact(args.artifact)
    if "default.xex" not in files:
        sys.exit("error: no default.xex in %s" % args.artifact)
    if any(name.split("/")[0] == "game" or name.endswith(".lua") for name in files):
        sys.exit("error: %s holds game files; release zips never do" % args.artifact)
    xex = files["default.xex"]
    digest = hashlib.sha256(xex).hexdigest()
    expected = files.get("default.xex.sha256", b"").split()
    if expected and expected[0].decode() != digest:
        sys.exit("error: default.xex doesn't match default.xex.sha256")

    top = "Balatro360-v%s" % args.version
    entries = [("Balatro360/default.xex", xex)]
    for name in sorted(files):
        if name.startswith("licenses/") or name == "THIRD_PARTY.md":
            entries.append(("Balatro360/" + name, files[name]))
    with open(os.path.join(RELEASE, "Convert Balatro.bat"), "rb") as f:
        entries.append(("Convert Balatro.bat", crlf(f.read())))
    with open(os.path.join(RELEASE, "convert.ps1"), "rb") as f:
        entries.append(("converter/convert.ps1", crlf(f.read())))
    with open(os.path.join(ROOT, "tools", "prepare_game.py"), "rb") as f:
        entries.append(("converter/prepare_game.py", f.read()))
    with open(os.path.join(RELEASE, "README.txt"), "rb") as f:
        readme = f.read().replace(b"{VERSION}", ("v" + args.version).encode())
        # Keep the underline as long as the title.
        lines = readme.split(b"\n")
        lines[1] = b"=" * len(lines[0])
        entries.append(("README.txt", crlf(b"\n".join(lines))))

    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, top + ".zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for name, data in entries:
            zf.writestr(top + "/" + name, data)
    with open(out, "rb") as f:
        zip_digest = hashlib.sha256(f.read()).hexdigest()
    print("default.xex  sha256 %s" % digest)
    print("%s  sha256 %s" % (os.path.basename(out), zip_digest))
    print("wrote %s (%d files)" % (out, len(entries)))


if __name__ == "__main__":
    main()
