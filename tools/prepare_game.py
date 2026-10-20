#!/usr/bin/env python3
"""Builds the Balatro360 game folder from your own copy of Balatro.

Balatro for PC is a LÖVE game: Balatro.exe is the LÖVE runtime with the
game's files appended as a zip archive. This script extracts that archive
into the folder layout the Xbox 360 build expects:

    <output>/
        default.xex      (copied from --xex if given)
        game/            (the game's Lua code and resources)
        save/            (created on the console on first run)

Usage:
    python tools/prepare_game.py "path/to/Balatro.exe" out/Balatro360
    python tools/prepare_game.py Balatro.exe out/Balatro360 --xex build/xbox360/out/Release/default.xex

The input may also be a .love file or an already-extracted game folder.
Only standard library modules are used (Python 3.6+).
"""
import argparse
import os
import shutil
import sys
import zipfile

# FATX (the 360's file system) limits file names to 42 characters.
FATX_MAX_NAME = 42

# Files that are only useful on PC and are skipped.
SKIP_SUFFIXES = (".dll", ".exe")


def find_game_files(source):
    """Returns (kind, handle) for a zip-containing file or a directory."""
    if os.path.isdir(source):
        if not os.path.exists(os.path.join(source, "main.lua")):
            sys.exit("error: %s does not contain main.lua" % source)
        return "dir", source
    if not os.path.isfile(source):
        sys.exit("error: %s not found" % source)
    try:
        # zipfile copes with data prepended to the archive (the LÖVE exe).
        zf = zipfile.ZipFile(source)
    except zipfile.BadZipFile:
        sys.exit("error: %s does not contain a LÖVE game archive" % source)
    if "main.lua" not in zf.namelist():
        sys.exit("error: %s has no main.lua; is this Balatro.exe?" % source)
    return "zip", zf


def iter_files(kind, handle):
    if kind == "zip":
        for info in handle.infolist():
            if info.is_dir():
                continue
            yield info.filename, lambda i=info: handle.read(i)
    else:
        for root, _, files in os.walk(handle):
            for name in files:
                full = os.path.join(root, name)
                relpath = os.path.relpath(full, handle).replace(os.sep, "/")
                yield relpath, lambda p=full: open(p, "rb").read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="Balatro.exe, a .love file, or an extracted game folder")
    ap.add_argument("output", help="output folder (will contain game/ and default.xex)")
    ap.add_argument("--xex", help="path to the built default.xex to copy into the output folder")
    ap.add_argument("--clean", action="store_true", help="delete output/game before extracting")
    args = ap.parse_args()

    kind, handle = find_game_files(args.source)
    game_dir = os.path.join(args.output, "game")
    if args.clean and os.path.isdir(game_dir):
        shutil.rmtree(game_dir)
    os.makedirs(game_dir, exist_ok=True)

    count = 0
    total = 0
    long_names = []
    for relpath, read in iter_files(kind, handle):
        if relpath.lower().endswith(SKIP_SUFFIXES):
            continue
        for part in relpath.split("/"):
            if len(part) > FATX_MAX_NAME:
                long_names.append(relpath)
        dest = os.path.join(game_dir, *relpath.split("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        data = read()
        with open(dest, "wb") as f:
            f.write(data)
        count += 1
        total += len(data)

    version = "unknown"
    vpath = os.path.join(game_dir, "version.jkr")
    if os.path.exists(vpath):
        version = open(vpath, "rb").read().decode("utf-8", "replace").splitlines()[0].strip()

    if args.xex:
        if not os.path.isfile(args.xex):
            sys.exit("error: %s not found" % args.xex)
        shutil.copyfile(args.xex, os.path.join(args.output, "default.xex"))

    print("Balatro version : %s" % version)
    print("Extracted       : %d files, %.1f MB -> %s" % (count, total / 1048576.0, game_dir))
    if args.xex:
        print("Copied XEX      : %s" % os.path.join(args.output, "default.xex"))
    if long_names:
        print("warning: these names exceed the FATX limit of %d characters and will not copy to the 360:"
              % FATX_MAX_NAME)
        for n in long_names:
            print("  " + n)
    print("")
    print("Copy the whole '%s' folder to your console (e.g. a USB drive) and" % args.output)
    print("launch default.xex. Saves and b360.log are written next to it.")


if __name__ == "__main__":
    main()
