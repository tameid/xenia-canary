#!/usr/bin/env python3
"""Flip the "Kinect Initialized" bit in a xenia xconfig.settings blob.

The emulator only lets you tick this through Console Settings -> Retail Options
(console_settings_dialog.cc:306-310). For the spike we want an A/B pair of
storage roots, so this writes the flag directly.

Layout: src/xenia/kernel/xconfig.h
  static_settings   0x000 .. 0x10E  (270 bytes)
  statistic         0x10E .. 0x05D8 (1496 bytes)
  secured           0x05D8 .. 0x0278 (512 bytes)
  user              0x0278 ..        user.retail_flags is at +0x038 -> 0x02B0 (2334)
  X_RETAIL_FLAGS::KinectInitialized = 0x20000000, KinectDisabled = 0x80000000

Usage:
  set-kinect-flag.py <xconfig.settings> on
  set-kinect-flag.py <xconfig.settings> off
  set-kinect-flag.py <xconfig.settings> show
"""

import struct
import sys

RETAIL_FLAGS_OFFSET = 2338 - 4  # verified against a default blob (0x00000040)
KINECT_INITIALIZED = 0x20000000
KINECT_DISABLED = 0x80000000


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[0]
    action = argv[1] if len(argv) > 1 else "show"

    with open(path, "r+b") as handle:
        handle.seek(RETAIL_FLAGS_OFFSET)
        raw = handle.read(4)
        if len(raw) != 4:
            print(f"{path}: too small to contain XConfigData", file=sys.stderr)
            return 1
        flags = struct.unpack(">I", raw)[0]

        if action == "show":
            print(f"retail_flags = 0x{flags:08X}")
            print(f"  KinectInitialized = {bool(flags & KINECT_INITIALIZED)}")
            print(f"  KinectDisabled    = {bool(flags & KINECT_DISABLED)}")
            return 0

        if action == "on":
            flags |= KINECT_INITIALIZED
            flags &= ~KINECT_DISABLED
        elif action == "off":
            flags &= ~KINECT_INITIALIZED
            flags &= ~KINECT_DISABLED
        else:
            print(f"unknown action {action!r}", file=sys.stderr)
            return 2

        handle.seek(RETAIL_FLAGS_OFFSET)
        handle.write(struct.pack(">I", flags))
        handle.flush()

    print(f"{path}: retail_flags = 0x{flags:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))