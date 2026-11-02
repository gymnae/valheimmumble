#!/usr/bin/env python3
"""Bundle built plugin libraries into a .mumble_plugin file (a zip with manifest.xml).

    python3 tools/package.py out/valheim_radio.mumble_plugin \
        --windows build-win/valheim_radio.dll --linux build-linux/libvalheim_radio.so

Mumble picks the library matching its OS/architecture on install.
"""
import argparse
import os
import zipfile

VERSION = "2.0.0"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--windows", help="x86_64 Windows DLL")
    ap.add_argument("--linux", help="x86_64 Linux .so")
    ap.add_argument("--macos", help="macOS .dylib (universal or x86_64)")
    args = ap.parse_args()

    entries = []
    if args.windows:
        entries.append(("windows", "x86_64", args.windows, "valheim_radio.dll"))
    if args.linux:
        entries.append(("linux", "x86_64", args.linux, "libvalheim_radio.so"))
    if args.macos:
        entries.append(("macos", "universal", args.macos, "libvalheim_radio.dylib"))
    if not entries:
        ap.error("give at least one library")

    assets = "\n".join(f'    <plugin os="{o}" arch="{a}">{name}</plugin>' for o, a, _, name in entries)
    manifest = f"""<?xml version="1.0" encoding="UTF-8"?>
<bundle version="1.0.0">
  <assets>
{assets}
  </assets>
  <name>Valheim</name>
  <version>{VERSION}</version>
</bundle>
"""
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.xml", manifest)
        for _, _, src, name in entries:
            z.write(src, name)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
