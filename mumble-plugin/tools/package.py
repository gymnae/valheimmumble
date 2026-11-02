#!/usr/bin/env python3
"""Bundle built plugin libraries into a .mumble_plugin file (a zip with manifest.xml).

    python3 tools/package.py out/valheim_radio.mumble_plugin \
        --lib windows:x64:build/windows-x64/valheim_radio.dll \
        --lib linux:arm64:build/linux-arm64/libvalheim_radio.so \
        --lib macos:x64,arm64,universal:build/macos-universal/libvalheim_radio.dylib

Mumble installs the library whose manifest entry matches its own MUMBLE_TARGET_OS
("windows", "linux", "macos") and MUMBLE_TARGET_ARCH ("x64", "x86", "arm64", "armv7", ...,
or "universal" for universal macOS builds). One library may serve several arch names.
"""
import argparse
import os
import zipfile

VERSION = "2.0.0"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--lib", action="append", required=True, metavar="OS:ARCH[,ARCH...]:PATH",
                    help="a library and the Mumble OS/arch names it serves; repeat for each")
    args = ap.parse_args()

    files = {}    # name in zip -> source path
    entries = []  # (os, arch, name in zip)
    for spec in args.lib:
        os_name, archs, path = spec.split(":", 2)
        base, ext = os.path.splitext(os.path.basename(path))
        name = f"{os_name}-{archs.split(',')[0]}/{base}{ext}"
        files[name] = path
        entries += [(os_name, arch, name) for arch in archs.split(",")]

    assets = "\n".join(f'    <plugin os="{o}" arch="{a}">{n}</plugin>' for o, a, n in entries)
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
        for name, src in files.items():
            z.write(src, name)
    print(manifest)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
