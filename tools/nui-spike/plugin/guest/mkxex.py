#!/usr/bin/env python3
"""
mkxex.py - wrap a flat Xenon image produced by xenon-gcc/xenon-ld into an
XEX2 container that xenia will load directly.

Everything here is derived from xenia-canary's own loader/headers:

  src/xenia/cpu/xex_module.cc          (XexModule::Load / LoadContinue /
                                        ReadPEHeaders / SetupLibraryImports)
  src/xenia/kernel/util/xex2_info.h    (xex2_header, xex2_security_info,
                                        xex2_opt_import_libraries,
                                        xex2_import_library, header key enum)
  src/xenia/base/pe_image.h            (XIMAGE_* constants)

No encryption and no compression are used: xenia honours
XEX_ENCRYPTION_NONE + XEX_COMPRESSION_NONE (xex_module.cc:540-596), so the
image bytes go in the file verbatim.

Usage:
  mkxex.py --flat flat.bin --flat-rva 0x1000 --base 0x82000000 \
           --entry 0x82001000 --stack-size 0x40000 --out nui-probe.xex
"""

import argparse
import struct

# ---- XEX2 optional header keys (src/xenia/kernel/util/xex2_info.h) ----------
K_FILE_FORMAT_INFO = 0x000003FF
K_IMAGE_BASE_ADDRESS = 0x00010201
K_ENTRY_POINT = 0x00010100
K_IMPORT_LIBRARIES = 0x000103FF
K_DEFAULT_STACK_SIZE = 0x00020200
K_TITLE_WORKSPACE_SIZE = 0x00040201

# ---- page descriptor section types -----------------------------------------
SEC_CODE = 1
SEC_DATA = 2
SEC_READONLY_DATA = 3

# ---- PE constants (src/xenia/base/pe_image.h) ------------------------------
PE_MACHINE_POWERPCBE = 0x01F2
PE_FILE_32BIT_MACHINE = 0x0100
PE_FILE_EXECUTABLE_IMAGE = 0x0002
PE_NT_OPTIONAL_HDR32_MAGIC = 0x10B
PE_SUBSYSTEM_XBOX = 14
PE_SIZEOF_NT_OPTIONAL_HEADER = 224

# The first 4 bytes on disk must literally spell "XEX2": Emulator::GetFileSignature
# (emulator.cc:494) builds a fourcc from file_magic[0..3] in that byte order.
# Written big-endian, that is the value 0x58455832.
XEX2_MAGIC = 0x58455832
XEX_MODULE_TITLE = 0x00000001

# The guest heap covering 0x80000000-0x8BFFFFFF has 64 KiB pages
# (src/xenia/memory.cc:216), and XexModule::LoadContinue walks
# xex2_security_info::page_descriptors using heap->page_size(). So the page
# descriptors count 64 KiB units, not 4 KiB ones.
PAGE_SIZE = 0x10000

# Import records: (library name, name_index, [(ordinal, thunk_address)])
#
# THESE MUST MATCH thunks.S. The addresses are the .ximports slots in link
# order, 16 bytes each starting at 0x82002000. If the two drift, the guest
# loads but every call lands on the wrong export - the kind of failure that
# costs an afternoon, and the reason the table lives in one place.
IMPORTS = [
    ("xboxkrnl.exe", 0, [
        (0x2BC, 0x820020A0),  # ExReadModifyWriteXConfigSettingUlong
        (0x003, 0x82002000),  # DbgPrint
        (0x0D2, 0x82002010),  # NtCreateFile
        (0x0F0, 0x82002020),  # NtReadFile
        (0x0CF, 0x82002030),  # NtClose
    ]),
    ("xam.xex", 1, [
        (0x885, 0x82002040),  # XamNuiGetDeviceStatus
        (0x890, 0x82002050),  # XamNuiIsDeviceReady
        (0x892, 0x82002060),  # XamNuiPlayerEngagementUpdate
        (0x893, 0x82002070),  # XamNuiSkeletonGetBestSkeletonIndex
        (0x28E, 0x82002080),  # XNotifyBroadcast
        (0x1F6, 0x82002090),  # XMsgSystemProcessCall
    ]),
    # The camera driver. Imported so the plugin can drive the sequence itself
    # and print every return value, rather than inferring the contract from what
    # a title happens to call. The dashboard registers a notify listener and
    # never reaches Create, so it cannot answer these questions.
    ("xboxkrnl.exe", 0, [
        (0x222, 0x820020B0),  # XUsbcamCreate
        (0x21A, 0x820020C0),  # XUsbcamSetCaptureMode
        (0x21C, 0x820020D0),  # XUsbcamSetConfig
        (0x21B, 0x820020E0),  # XUsbcamGetConfig
        (0x220, 0x820020F0),  # XUsbcamSetView
        (0x21D, 0x82002100),  # XUsbcamGetState
        (0x21E, 0x82002110),  # XUsbcamReadFrame
        (0x223, 0x82002120),  # XUsbcamDestroy
    ]),
]

# (info, page_count) in 64 KiB units, covering the whole image from base+0.
# Page 0 holds the PE headers, .text, .ximports (the import thunks, which xenia
# rewrites with executable code) and .rodata, so it must be CODE: the
# executable range xenia commits is derived from the CODE pages alone
# (xex_module.cc:1065-1082).
# Page 1 holds .data and must be DATA so it stays writable.
PAGE_LAYOUT = [
    (SEC_CODE, 1),
    # Six data pages, not one. The camera frame buffer is 0x4B000 (640*480, one
    # byte per pixel - the size xenia's own XUsbcamCreate documents), and .bss
    # lives after .data, so it needs pages of its own. One page silently truncated
    # the buffer: the guest ran the camera probe, then stopped dead before its
    # first post-camera print, with no fault raised anywhere in the log.
    (SEC_DATA, 6),
]

SECTIONS = [
    # name,      vaddr,  vsize,   rawsize,  characteristics
    (".text",    0x01000, 0x01000, 0x01000, 0x60000020),
    (".ximport", 0x02000, 0x01000, 0x01000, 0x60000020),
    (".rodata",  0x03000, 0x01000, 0x01000, 0x40000040),
    (".data",    0x10000, 0x10000, 0x10000, 0xC0000040),
]


def pad4(b):
    return b + b"\x00" * ((4 - len(b) % 4) % 4)


def build_import_libraries():
    """xex2_opt_import_libraries payload."""
    strings = b""
    for name, _, _ in IMPORTS:
        strings += pad4(name.encode("ascii") + b"\x00")

    out = b""
    out += struct.pack(">II", 0, len(strings))   # placeholder size, string_table.size
    out += struct.pack(">I", len(IMPORTS))      # string_table.count
    out += strings

    libs = b""
    for name, name_index, entries in IMPORTS:
        size = 0x28 + 4 * len(entries)
        lib = struct.pack(">I", size)
        lib += b"\x00" * 0x14                    # next_import_digest (unverified)
        lib += struct.pack(">I", 0)             # id
        lib += struct.pack(">I", 0)             # version
        lib += struct.pack(">I", 0)             # version_min
        lib += struct.pack(">HH", name_index, len(entries))
        for ordinal, addr in entries:
            lib += struct.pack(">I", addr)
        assert len(lib) == size
        libs += lib

    out += libs
    out = struct.pack(">I", len(out)) + out[4:]
    return out


def build_file_format_info():
    """xex2_opt_file_format_info: unencrypted, uncompressed."""
    return struct.pack(">III", 8, 0, 0)  # info_size, encryption NONE, compression NONE


def build_pe(image_size):
    """DOS + NT headers + section headers, written into the image at offset 0."""
    e_lfanew = 0x100
    dos = bytearray(0x100)
    struct.pack_into("<H", dos, 0x00, 0x5A4D)       # e_magic 'MZ'
    # XexModule::is_valid_executable() compares the first *host-order* dword
    # against 0x00905A4D, so byte 2 must be 0x90 (the usual DOS e_cblp).
    dos[0x02] = 0x90
    struct.pack_into("<I", dos, 0x3C, e_lfanew)     # e_lfanew

    num_sections = len(SECTIONS)
    file_hdr = struct.pack("<HHIIIHH",
                           PE_MACHINE_POWERPCBE,
                           num_sections,
                           0x00000000,             # TimeDateStamp
                           0, 0,                   # symbol table
                           PE_SIZEOF_NT_OPTIONAL_HEADER,
                           PE_FILE_EXECUTABLE_IMAGE | PE_FILE_32BIT_MACHINE)

    size_of_code = sum(1 for s in SECTIONS if s[4] & 0x20) * 0x1000
    size_of_init_data = sum(1 for s in SECTIONS if s[4] & 0x40) * 0x1000
    opt = struct.pack("<HBBIIIIII",
                      PE_NT_OPTIONAL_HDR32_MAGIC, 0, 0,
                      size_of_code, size_of_init_data, 0,
                      SECTIONS[0][1],             # AddressOfEntryPoint (patched later)
                      SECTIONS[0][1],             # BaseOfCode
                      SECTIONS[3][1])             # BaseOfData
    opt += struct.pack("<III", 0, 0x1000, 0x200)   # ImageBase, SectionAlign, FileAlign
    opt += struct.pack("<HHHHHH", 4, 0, 0, 0, 4, 0)
    opt += struct.pack("<III", 0, image_size, 0x1000)  # Win32Ver, SizeOfImage, SizeOfHeaders
    opt += struct.pack("<IHHIIII", 0, PE_SUBSYSTEM_XBOX, 0,
                       0x100000, 0x10000, 0, 0x100000)
    opt += struct.pack("<II", 0, 16)               # LoaderFlags, NumberOfRvaAndSizes
    opt += b"\x00" * (16 * 8)                      # DataDirectory[16] (unused by xenia)
    assert len(opt) == PE_SIZEOF_NT_OPTIONAL_HEADER, len(opt)

    sec_hdrs = b""
    for name, vaddr, vsize, rawsize, chars in SECTIONS:
        sec_hdrs += struct.pack("<8sIIIIIIHHI",
                                name.encode("ascii").ljust(8, b"\x00"),
                                vsize, vaddr, rawsize, vaddr,
                                0, 0, 0, 0, chars)
    assert len(sec_hdrs) == num_sections * 40

    nt = struct.pack("<I", 0x00004550) + file_hdr + opt + sec_hdrs
    dos[e_lfanew:e_lfanew + len(nt)] = nt
    return bytes(dos)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flat", required=True)
    ap.add_argument("--flat-rva", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--entry", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--stack-size", type=lambda x: int(x, 0), default=0x40000)
    ap.add_argument("--workspace-size", type=lambda x: int(x, 0), default=0x80000)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    total_pages = sum(c for _, c in PAGE_LAYOUT)
    image_size = total_pages * PAGE_SIZE

    with open(args.flat, "rb") as f:
        flat = f.read()

    # --- flat image -------------------------------------------------------
    image = bytearray(image_size)
    image[args.flat_rva:args.flat_rva + len(flat)] = flat

    pe = build_pe(image_size)
    # AddressOfEntryPoint is the RVA of _start.
    image[0x100:0x104 + 20 + 224] = pe[0x100:0x100 + 4 + 20 + 224]
    struct.pack_into("<I", image, 0x100 + 4 + 20 + 16, args.entry - args.base)
    image[0:len(pe)] = pe

    # --- xex2 security info ----------------------------------------------
    sec_size = 0x184 + 0x18 * len(PAGE_LAYOUT)
    sec = struct.pack(">II", sec_size, image_size)
    sec += b"\x00" * 0x100          # rsa_signature
    sec += struct.pack(">I", 0)    # info_size
    sec += struct.pack(">I", 0)    # image_flags
    sec += struct.pack(">I", args.base)  # load_address
    sec += b"\x00" * 0x14          # section_digest
    sec += struct.pack(">I", len(IMPORTS))  # import_table_count
    sec += b"\x00" * 0x14          # import_table_digest
    sec += b"\x00" * 0x10          # xgd2_media_id
    sec += b"\x00" * 0x10          # aes_key
    sec += struct.pack(">I", 0)    # export_table
    sec += b"\x00" * 0x14          # header_digest
    sec += struct.pack(">I", 0)    # region
    sec += struct.pack(">I", 0)    # allowed_media_types
    sec += struct.pack(">I", len(PAGE_LAYOUT))
    for info, count in PAGE_LAYOUT:
        sec += struct.pack(">I", (info & 0xF) | (count << 4))
        sec += b"\x00" * 0x14      # data_digest
    assert len(sec) == sec_size, (len(sec), sec_size)

    ffi = build_file_format_info()
    imports = build_import_libraries()

    # --- xex2 header ------------------------------------------------------
    # inline (value-in-key) entries first, then payload entries with offsets.
    opt_headers = []   # (key, inline_value_or_None, payload_or_None)

    opt_headers.append((K_IMAGE_BASE_ADDRESS, args.base, None))
    opt_headers.append((K_ENTRY_POINT, args.entry, None))
    opt_headers.append((K_DEFAULT_STACK_SIZE, args.stack_size, None))
    opt_headers.append((K_TITLE_WORKSPACE_SIZE, args.workspace_size, None))
    opt_headers.append((K_FILE_FORMAT_INFO, None, ffi))
    opt_headers.append((K_IMPORT_LIBRARIES, None, imports))

    head = 0x18 + 8 * len(opt_headers)
    payload_base = head
    placed = []
    for key, inline, payload in opt_headers:
        if payload is None:
            placed.append((key, inline, None))
        else:
            placed.append((key, payload_base, payload))
            payload_base += len(payload)

    security_offset = payload_base
    header_size = security_offset + len(sec)
    header_size = (header_size + 3) & ~3

    hdr = bytearray(header_size)
    struct.pack_into(">IIIII", hdr, 0,
                     XEX2_MAGIC, XEX_MODULE_TITLE, header_size, 0, security_offset)
    struct.pack_into(">I", hdr, 0x14, len(opt_headers))
    for i, (key, value, payload) in enumerate(placed):
        # For payload entries `value` is the offset of the data, relative to
        # the start of the xex2 header (xex_module.cc GetOptHeader default case).
        struct.pack_into(">II", hdr, 0x18 + i * 8, key, value)

    # Lay the payloads out back to back right after the header entry table.
    pos = 0x18 + 8 * len(opt_headers)
    for key, value, payload in placed:
        if payload is not None:
            hdr[pos:pos + len(payload)] = payload
            pos += len(payload)
    hdr[security_offset:security_offset + len(sec)] = sec

    with open(args.out, "wb") as f:
        f.write(bytes(hdr))
        f.write(bytes(image))

    print("wrote %s" % args.out)
    print("  header_size   = 0x%X" % header_size)
    print("  image_size    = 0x%X (base 0x%08X, entry 0x%08X)" %
          (image_size, args.base, args.entry))
    print("  security_off  = 0x%X" % security_offset)
    print("  opt headers   = %d" % len(opt_headers))


if __name__ == "__main__":
    main()