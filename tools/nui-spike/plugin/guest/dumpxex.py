#!/usr/bin/env python3
"""
dumpxex.py - print the import table of a real XEX.

Written by inverting the loader, not by guessing. The structures are from
src/xenia/kernel/util/xex2_info.h and the walk order from
src/xenia/cpu/xex_module.cc:SetupLibraryImports (xex_module.cc:1223-1297):

  * xex2_opt_import_libraries: {size, string_table:{size, count, data}}
  * xex2_import_library: size, next_import_digest[0x14], id, version,
    version_min, name_index@0x24, count@0x26, import_table[]@0x28
  * import_table[i] is a *record address* (a guest virtual address), not an
    ordinal. The ordinal is in the low 16 bits of the 32-bit value stored
    there, and bits 24-31 are the record type (0 = import variable).
    That is a second indirection and it is easy to miss - the import_table
    itself contains no ordinals at all.

Purpose: xenia's export tables show what the emulator *offers*. This shows what
a title that genuinely uses NUI actually *asks for*, which is the question the
source cannot answer.

Usage:
  dumpxex.py <file.xex> [--xam] [--krnl] [--filter 0x8xx]
"""

import argparse
import json
import struct
import sys

K_IMPORT_LIBRARIES = 0x000103FF
XEX2_MAGIC = 0x58455832

HEADER_SIZE_OFF = 0x08
SECURITY_OFF_OFF = 0x10
OPT_COUNT_OFF = 0x14
OPT_TABLE_OFF = 0x18


def be32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def be16(d, o):
    return struct.unpack_from(">H", d, o)[0]


def parse(path):
    with open(path, "rb") as f:
        data = f.read()

    magic, _mt, _hs, _, security_off = struct.unpack_from(">IIIII", data, 0)
    if magic != XEX2_MAGIC:
        raise SystemExit("not a bare XEX2 (magic 0x%08X)" % magic)

    opt = {}
    n = be32(data, OPT_COUNT_OFF)
    for i in range(n):
        k, v = struct.unpack_from(">II", data, OPT_TABLE_OFF + i * 8)
        opt[k] = v
    base = opt.get(0x00010201)

    # The image is the flat Xenon image that follows the xex2 header and the
    # security info block, in that order (see mkxex.py's layout, which is the
    # inverse of this).
    si_size = be32(data, security_off)
    image_off = security_off + si_size

    def read_va(va):
        """The value stored in the import record slot at guest VA `va`."""
        if base is None:
            raise SystemExit("no image base address header")
        fo = image_off + (va - base)
        return be32(data, fo)

    libs_off = opt.get(K_IMPORT_LIBRARIES)
    if libs_off is None:
        raise SystemExit("no import libraries header (key 0x103FF)")

    _total = be32(data, libs_off)
    str_size = be32(data, libs_off + 4)
    str_count = be32(data, libs_off + 8)

    p = libs_off + 12
    strings = {}
    for i in range(str_count):
        nul = data.find(b"\x00", p)
        strings[i] = data[p:nul].decode("ascii", "replace")
        p += ((nul - p) + 1 + 3) & ~3

    lib_count = be32(data, p)
    p += 4

    result = {}
    for _ in range(lib_count):
        lib_size = be32(data, p)
        name_index = be16(data, p + 0x24)
        count = be16(data, p + 0x26)

        funcs, variables = [], []
        for i in range(count):
            rec_addr = be32(data, p + 0x28 + i * 4)
            if rec_addr == 0:
                continue
            val = read_va(rec_addr)
            rec_type = (val & 0xFF000000) >> 24
            ordinal = val & 0xFFFF
            (funcs if rec_type == 0 else variables).append(ordinal)

        result[strings.get(name_index, "?%d" % name_index)] = (funcs, variables)
        p += lib_size

    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--xam", action="store_true", help="only xam.xex function imports")
    ap.add_argument("--krnl", action="store_true", help="only xboxkrnl.exe function imports")
    ap.add_argument("--filter", type=lambda s: int(s, 0),
                    help="only ordinals in [lo, lo+256)")
    ap.add_argument("--json", help="write full result here")
    args = ap.parse_args()

    libs = parse(args.path)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({k: {"functions": v[0], "variables": v[1]}
                       for k, v in libs.items()}, f, indent=1)

    for name, (funcs, variables) in libs.items():
        if args.xam and not name.startswith("xam"):
            continue
        if args.krnl and not name.startswith("xboxkrnl"):
            continue

        sel = funcs
        if args.filter is not None:
            sel = [o for o in funcs if args.filter <= o < args.filter + 0x100]
        if (args.xam or args.krnl or args.filter is not None) and not sel:
            continue

        print("%s: %d function imports, %d variables" %
              (name, len(funcs), len(variables)))
        if args.filter is not None:
            for o in sel:
                print("    0x%04X" % o)
        else:
            print("    " + " ".join("%03X" % o for o in sorted(funcs)))


if __name__ == "__main__":
    main()