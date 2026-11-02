#!/usr/bin/env python3
"""Minimal `lipo -create`: merge thin Mach-O files into one universal (fat) binary.

    python3 tools/lipo.py out.dylib x86_64.dylib arm64.dylib
"""
import struct
import sys

FAT_MAGIC = 0xCAFEBABE
MH_MAGIC_64 = 0xFEEDFACF
ALIGN = 14  # 2^14 = 16 KiB, what Apple's lipo uses for arm64


def main():
    out, *inputs = sys.argv[1:]
    thin = []
    for path in inputs:
        data = open(path, "rb").read()
        magic, cputype, cpusubtype = struct.unpack("<III", data[:12])
        if magic != MH_MAGIC_64:
            sys.exit(f"{path}: not a 64-bit little-endian Mach-O file")
        thin.append((cputype, cpusubtype, data))

    header = struct.pack(">II", FAT_MAGIC, len(thin))
    offset = 1 << ALIGN
    arch_table, blobs = b"", b""
    for cputype, cpusubtype, data in thin:
        arch_table += struct.pack(">IIIII", cputype, cpusubtype, offset, len(data), ALIGN)
        padded = data + b"\0" * (-len(data) % (1 << ALIGN))
        blobs += padded
        offset += len(padded)
    head = header + arch_table
    with open(out, "wb") as f:
        f.write(head + b"\0" * ((1 << ALIGN) - len(head)) + blobs)


if __name__ == "__main__":
    main()
