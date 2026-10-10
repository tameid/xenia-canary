# NUI guest plugin (Phase 2, in progress)

The guest half of the Kinect pipeline: a homebrew `.xex` that xenia loads from
`storage_root()/plugins/<TITLE_ID>/`, reads skeleton frames published by
[tameid/nui-provider](https://github.com/tameid/nui-provider), and impersonates a
Kinect to the rest of the system.

## Status

| piece | state |
|---|---|
| wire-format decode (byte-swapped) | **done and tested on the host** |
| IPC polling (`frame.idx` → `frame.N.bin`) | next |
| skeleton ring buffer | next |
| impersonating the device to xam | blocked on the 360 toolchain and the `xam.xex` import question |
| packaging into a `.xex` | blocked on the 360 toolchain |

## What is here now, and why it is already worth having

```
include/nui_guest.h     the wire format, re-declared for the guest
src/nui_decode.c        byte-swapped decode of a frame and its skeletons
tests/test_decode.c     host test for the above
```

The provider and this plugin are separate repositories running on different
processors, so the layout is **duplicated deliberately** — there is no shared
header to include. Duplication is the risk, and it is managed by:

* `static_assert`-equivalent size macros here that mirror the provider's, and
* `tests/test_decode.c`, which compiles this exact code on the host and checks
  it against frames **actually produced by nui-provider**.

If the provider ever changes a layout without a version bump, that test fails
rather than the guest silently reading plausible-looking wrong poses.

This is the highest-risk part of the plugin, and it does not need a 360
toolchain to verify. Only the `.xex` packaging waits.

## Building the host test

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64
cd tools\nui-spike\plugin\build
cl /nologo /W4 /WX /std:c17 /I ..\include ..\src\nui_decode.c ..\tests\test_decode.c /Fe:test_decode.exe
```

Then, with some frames in hand:

```bat
..\..\..\..\..\nui-provider\build\bin\nui-provider.exe --dir %TEMP%\plugtest --skeletons 2 --frames 5
test_decode.exe %TEMP%\plugtest\frame.0.bin %TEMP%\plugtest\frame.1.bin
```

It also accepts a `.nui` session and validates its first frame, so a recording
can be used as the fixture.

## Building the `.xex`

Blocked on the Xbox 360 toolchain. This is the main practical blocker the
design doc flags, and the open question it carries is whether an import stub is
available for `xam.xex` or only `xboxkrnl.xex`. That determines whether the
plugin can call `XMsgSystemProcessCall(0xFE, 0x2Bxxx, ...)` directly or has to
take another route to impersonate the device.

Once that is resolved the plugin needs, in order:

1. **IPC polling.** Open `frame.idx`, read the slot it names, read
   `frame.N.bin`. The writer never mutates the slot being read, so a torn read
   is only possible if the read straddles a publish — which is why the decoder
   validates magic, version and count rather than trusting them.
2. **Skeleton ring buffer.** Skeletons are copied into guest memory,
   double-buffered, newest frame winning.
3. **Device impersonation.** Deliver to xam and broadcast
   `kXNotificationSystemNUIHardwareStatusChanged` on connect/disconnect.

## Note on the decode

The guest is a big-endian PowerPC and the provider writes little-endian, so
every field is byte-swapped on read — `NuiReadU16/U32/U64/F32` in the header. The
float reader swaps through an integer and uses plain `memcpy` rather than a
compiler builtin, so the same source builds under the GCC-based 360 toolchain
and under MSVC for the host test.

Skeletons are decoded with every joint. Skeleton *orientation* is deliberately
not decoded: nothing consumes it yet, and it should be added with a test if a
title needs it rather than speculatively.