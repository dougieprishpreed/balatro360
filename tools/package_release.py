#!/usr/bin/env python3
"""Builds the release from the GitHub Actions build, packed for the Homebrew App Store. No game data goes in.

    python3 tools/package_release.py ARTIFACT --xsdk XSDK [--version 1.0.0.0] [--out out/release]

ARTIFACT is the "balatro360-xex" artifact of the xSDK build workflow, as its zip or the folder it was extracted to
(default.xex, default.xex.sha256, licenses/, THIRD_PARTY.md). XSDK is an xenosworks/xsdk checkout, for its
tools/store_catalogue.py. The version defaults to tools/release/VERSION, the XEX's own. It writes:

    balatro360-<version>.zip            the package the store installs into hdd:\\apps\\balatro360\\, made by
                                        store_catalogue.py pack (upload it exactly as it is)
    Balatro360-Xbox360-<version>.zip    for installing by hand: apps\\balatro360\\ and README.txt
    balatro360-<version>.json           the app's catalogue entry, with the package's size and hashes, for the
                                        pull request to xenosworks/homebrew-app-catalogue

The app folder holds default.xex, LICENSE.txt, THIRD_PARTY.md and licenses/. The player adds Balatro.exe, which
the first start unpacks (src/runtime/game_unpack.h).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE = os.path.join(ROOT, "tools", "release")
APP = "balatro360"
REPO = "https://github.com/dougieprishpreed/balatro360"
# Fixed dates, so the same files make the same zip.
ZIP_TIME = (2026, 1, 1, 0, 0, 0)


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
    """Text with Windows line endings, for Notepad."""
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("artifact", help="the balatro360-xex artifact (zip or folder)")
    ap.add_argument("--xsdk", required=True, help="xenosworks/xsdk checkout (tools/store_catalogue.py)")
    ap.add_argument("--version", help="release version, major.minor.build.fix (default: tools/release/VERSION)")
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "release"), help="folder for the release files")
    args = ap.parse_args()
    if not args.version:
        with open(os.path.join(RELEASE, "VERSION")) as f:
            args.version = f.read().strip()

    files = read_artifact(args.artifact)
    if "default.xex" not in files:
        sys.exit("error: no default.xex in %s" % args.artifact)
    if any(name.split("/")[0] == "game" or name.endswith(".lua") or name.lower().endswith("balatro.exe")
           for name in files):
        sys.exit("error: %s holds game files; releases never do" % args.artifact)
    xex = files["default.xex"]
    digest = hashlib.sha256(xex).hexdigest()
    expected = files.get("default.xex.sha256", b"").split()
    if expected and expected[0].decode() != digest:
        sys.exit("error: default.xex doesn't match default.xex.sha256")

    # The app's folder, as it goes in hdd:\apps\balatro360\.
    app_files = [("default.xex", xex)]
    with open(os.path.join(ROOT, "LICENSE"), "rb") as f:
        app_files.append(("LICENSE.txt", crlf(f.read())))
    for name in sorted(files):
        if name.startswith("licenses/") or name == "THIRD_PARTY.md":
            app_files.append((name, files[name]))

    os.makedirs(args.out, exist_ok=True)
    package = os.path.join(args.out, "%s-%s.zip" % (APP, args.version))
    by_hand = os.path.join(args.out, "Balatro360-Xbox360-%s.zip" % args.version)
    entry_path = os.path.join(args.out, "%s-%s.json" % (APP, args.version))
    url = "%s/releases/download/v%s/%s" % (REPO, args.version, os.path.basename(package))

    with tempfile.TemporaryDirectory() as work:
        folder = os.path.join(work, APP)
        for name, data in app_files:
            path = os.path.join(folder, *name.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        # pack fills in the entry in a catalogue holding it alone.
        with open(os.path.join(RELEASE, "store_entry.json")) as f:
            entry = json.load(f)
        catalogue = {"format": 1, "serial": 0, "url": "https://xenosworks.github.io/homebrew-app-catalogue/catalogue.json",
                     "categories": [{"id": entry["category"], "name": entry["category"], "sentence": "-"}],
                     "apps": [entry]}
        catalogue_path = os.path.join(work, "catalogue.json")
        with open(catalogue_path, "w") as f:
            json.dump(catalogue, f)
        tool = os.path.join(args.xsdk, "tools", "store_catalogue.py")
        result = subprocess.run([sys.executable, tool, "pack", catalogue_path, APP, folder, package, "--url", url],
                                capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        # The screenshot's SHA-256 is written when xenosworks signs the catalogue.
        problems = [line for line in result.stdout.splitlines()
                    if line.startswith("store_catalogue: app ") and "screenshot_sha256" not in line]
        if result.returncode != 0 or problems:
            sys.exit("error: store_catalogue.py pack failed")
        with open(catalogue_path) as f:
            entry = json.load(f)["apps"][0]
    if entry["version"] != args.version:
        sys.exit("error: default.xex is version %s, not %s" % (entry["version"], args.version))
    with open(entry_path, "w") as f:
        json.dump(entry, f, indent=1)
        f.write("\n")

    with open(os.path.join(RELEASE, "README.txt"), "rb") as f:
        readme = f.read().replace(b"{VERSION}", args.version.encode())
    # Keep the underline as long as the title.
    lines = readme.split(b"\n")
    lines[1] = b"=" * len(lines[0])
    entries = [("README.txt", crlf(b"\n".join(lines)))]
    entries += [("apps/%s/%s" % (APP, name), data) for name, data in app_files]
    with zipfile.ZipFile(by_hand, "w") as zf:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zf.writestr(info, data, compresslevel=9)

    print("default.xex  sha256 %s" % digest)
    for path in (package, by_hand):
        print("%s  sha256 %s" % (os.path.basename(path), sha256_file(path)))
    print("wrote %s, %s and %s" % (package, by_hand, entry_path))
    print("upload both zips to the release tagged v%s; the package's address is %s" % (args.version, url))


if __name__ == "__main__":
    main()
