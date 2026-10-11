# Kinect (NUI) support — design plan

**Status:** Phase 0 spike **complete**; design otherwise unchanged
**Constraint:** keep the fork as close to upstream `xenia-canary` as possible. Everything
that can live outside the emulator tree *does*.

> **Headline result (runbook §8):** the dashboard's NUI init was gated on
> `XamNuiIsDeviceReady`, a **host-side** export that always returned 0 because of a bug at
> `xam_nui.cc:199`. Guest patches and guest plugins cannot influence it, so a one-line
> in-tree fix was unavoidable. **That fix (patch 0003) is now written, built and measured:
> it is sufficient.** With it applied the dashboard stops polling the NUI surface and
> proceeds. The hardware pipeline itself still lives entirely outside the fork — the
> divergence is one line, default-off, and reads as an upstream bug fix.

**Running the spike:** see [`kinect_spike_runbook.md`](kinect_spike_runbook.md) for the
Windows host procedure, and [`tools/nui-spike/README.md`](../tools/nui-spike/README.md)
for the tooling, podman build environment and Linux/Proton path.

---

## 0. Summary

The plan is a three-layer, out-of-tree stack:

```
┌──────────────────────────────────────────────────────────────────────────┐
│ L3  nui-provider        external application, separate repo/process      │
│     backends: hardware | phone | webcam | replay | synthetic              │
│     pose estimation, calibration, smoothing   → emits guest-shaped       │
│     skeleton frames at 30 Hz                                               │
└───────────────▲──────────────────────────────────────────┬───────────────┘
                │ IPC (double-buffered file, or UDP)      │ writes
                │                                         ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ L2  kinect plugin      homebrew guest XEX, storage_root()/plugins/<TID>/ │
│     guest thread reads frames, impersonates the Kinect device to xam     │
│     (XMsgSystemProcessCall 0x2Bxxx/0x2Cxxx, XNotifyBroadcast 0x00060019) │
│     and/or feeds the title's own NUI entry points                        │
└───────────────▲──────────────────────────────────────────┬───────────────┘
                │ loaded by xenia's existing plugin_loader │ gated by
                │ (no xenia source change)                  ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ L1  patches             data-only guest patches, storage_root()/patches/ │
│     *.patch.toml, title-id + module-hash gated (existing patcher)        │
│     neutralize failure paths, redirect title NUI calls, embed constants  │
└──────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────┐
│ L0  xenia (upstream)    unchanged c2 except an explicitly-optional,      │
│                         default-off fallback PR (see §8)                  │
└──────────────────────────────────────────────────────────────────────────┘
```

Only L3 and the IPC file format are new *software*; L1/L2 use xenia mechanisms that already
exist and are already shipped. Realistic outcome: **zero upstream diff** for the whole
hardware/phone pipeline, with a small, isolated PR held in reserve for the few things that
genuinely cannot be done from guest code.

---

## 1. What xenia already gives us (verified in this tree)

These findings are what make the out-of-tree approach viable.

| Fact | Where |
|---|---|
| xenia already *advertises* a Kinect: the console-settings dialog has a **"Kinect Initialized"** checkbox that writes `X_RETAIL_FLAGS::KinectInitialized` (`0x20000000`) | `src/xenia/app/console_settings_dialog.cc:306-310`, `src/xenia/kernel/xconfig.h:462-464` |
| With that flag set, `XamNuiGetDeviceStatus` returns `X_ERROR_SUCCESS` and a "connected" status | `src/xenia/kernel/xam/xam_nui.cc:47-71` |
| Guest → xam message injection is implemented: `XMsgInProcessCall`, `XMsgSystemProcessCall`, `XMsgStartIORequest(Ex)` → `AppManager::DispatchMessageSync/Async` | `src/xenia/kernel/xam/xam_msg.cc:23-80`, `app_manager.h:54-57` |
| xam receives the Kinect message ranges **0x2Bxxx (camera) / 0x2Cxxx (identity)** in the xam app (id `0xFE`) | `src/xenia/kernel/xam/apps/xam_app.cc:20-21`, `:150-176` |
| `XNotifyBroadcast` works, and the NUI notification ids exist (none are broadcast yet) | `src/xenia/kernel/xam/xam_notify.cc:109`, `src/xenia/xbox.h:217-223` |
| **Patches**: data-only guest patches from TOML, keyed by title id + module hash, write raw bytes at guest addresses (`be8/16/32/64`, arrays, strings) | `src/xenia/patcher/patch_db.h:56-134`, `patcher.cc` |
| **Plugins**: homebrew *guest* XEX modules loaded from `storage_root()/plugins/<TITLE_ID>/`, hash-gated, behind `allow_plugins` cvar; the whole host directory is mounted into the guest as `plugins:` | `src/xenia/patcher/plugin_loader.cc:16-21, 58-127, 173-253` |
| Patches/plugins roots | `src/xenia/emulator.cc:309, 325-326` (`storage_root()/patches`, `storage_root()/plugins`) |
| Vendored deps usable from the provider app | `third_party/ffmpeg`, `libusb`, `SDL2`, `catch` |
| ImGui overlay available for debugging | `src/xenia/ui/imgui_drawer.h:43,65,93` |
| Unit-test scaffolding (Catch2) | `src/xenia/base/testing/*_test.cc` (`xe_test_suite`) |

### Known gap found during review — now measured, and it is the blocker

`XamNuiIsDeviceReady` (`xam_nui.cc:185-201`) computes `kinect_initialized >> 1 & 1`, which is
`0` for `0x20000000` — i.e. it always reports *not ready*, even with the checkbox on.
That makes `XamNuiHudIsEnabled` (`:272-285`) false and `XamNuiIdentityEnrollForSignIn`
(`:327-341`) fail. So:

* "Kinect present" is already obtainable with zero code, but
* "Kinect ready / identity" is not, and must be handled by patch, plugin, or the fallback PR (§8).

**Phase 0 confirmed this is the hard gate.** With the Kinect flag set *and*
`--nui_fake_device` set — so that `XamNuiGetDeviceStatus` returns `0` and
`XUsbcamGetState` returns `0x3` — the dashboard still spins indefinitely on
`XamNuiIsDeviceReady`, which returned `0` on all 1096 calls. Note `kinect_initialized` is
a `bool`, so `>> 1` is meaningless in the first place: **no retail-flag value makes this
return ready.** Since it is a host export, no guest-side patch or plugin can change it.
This is what forces the one-line §8 fix.

### Second gap: 91 of 129 NUI ordinals have no shim at all

`tools/nui-spike/gen_surface.py` parses the ordinal tables and every `DECLARE_*_EXPORT*` tag
(no build needed; regenerate any time upstream changes):

```sh
python3 tools/nui-spike/gen_surface.py --csv tools/nui-spike/nui_surface.csv
```

| status | count | meaning |
|---|---|---|
| implemented | 13 | has a working shim |
| stub | 21 | shim exists, returns a stubbed value |
| sketchy | 4 | implemented, known to misbehave |
| **not-declared** | **91** | ordinal is in the table but no shim exists |

For a not-declared ordinal, `KernelModule::GetProcAddressByOrdinal` logs
"fn export ... is not implemented" and **returns 0** (`kernel_module.cc:119-134`), i.e. the
guest receives a null import. Any NUI title that calls one of those 91 will fault rather
than fail gracefully.

### Third gap, measured: the three NUI device requests

NUIView (the Xbox 360 Kinect developer test app) imports `PsCamDeviceRequest` (0x30D),
`McaDeviceRequest` (0x30E) and `DetroitDeviceRequest` (0x30F) — all present in
`xboxkrnl_table.inc:795-797`, none implemented, all three null imports. The guest logged
`undefined extern call` for each.

These are also the functions xam's own NUI code documents as its device probes
(`xam_nui.cc:28-33`, `:150-158`), where a negative return is the "no device" signal.
`tools/nui-spike/0002-nui-device-request-stubs.patch` implements them (~90 lines, one new
file + one line of group registration) with an `nui_fake_device` cvar to report the device
as present for tracing. **This is the smallest possible instance of the §8 fallback PR
and a good candidate for upstream**: three stubs, additive, no behaviour change for
existing users.

### How to get a title's surface without running it

xenia dumps a module's import table on load, marking unimplemented exports with `!!`
(`user_module.cc:1199`). That dump *is* the title's required surface and is available
from any run that reaches `Loading module`. For NUIView the entire NUI surface is five
functions: `XamNuiIdentityGetSessionId` (implemented), `XamUserNuiGetUserIndex` (stub),
and the three device requests above (null imports until patch 0002). No live tracing
needed to enumerate a title's requirements — use the trace only for *behaviour*
(what arguments, what cadence, what messages).

---

## 2. Non-goals

* Face / biometric identity matching (`XamNuiIdentity*` will be made to *succeed plausibly*,
  not to identify anyone).
* Virtual USB device emulation (canary has no xusb stack; building one is a separate
  multi-month project). Consequence: we do **not** feed the guest's own KinectFusion pipeline;
  we feed the pipeline's *output*.
* Kinect v2 / Xbox One Kinect protocol support.
* Anything requiring redistribution of Kinect firmware blobs.

---

## 3. L3 — `nui-provider` (external application)

Separate repository, separate process, no xenia headers required. Written in C++17/20
(reuses the same third_party FFmpeg / libusb that xenia already vendors, so it can be
vendored the same way) with a Python fallback for rapid prototyping.

### 3.1 Backends

| Backend | Source | Notes |
|---|---|---|
| `synthetic` | scripted animation / replay file | deterministic; CI + smoke tests; no hardware |
| `webcam` | local camera via FFmpeg (`avdevice`/`dshow`) | reference implementation of the pose path |
| `phone` | phone camera | two variants, §3.3 |
| `hardware` | real Kinect via libfreenect (v1) | §3.4 |
| `replay` | recorded `.nui` sessions | A/B testing, bug repro |

### 3.2 Output contract (what every backend must emit)

Exactly the guest-facing shape, at 30 Hz, latest-frame-wins (drop, never queue):

```
NuiFrameHeader {
  u32 magic 'XNUI', u16 version, u16 flags,
  u32 frame_id, u64 capture_timestamp_us, u64 host_timestamp_us,
  u32 num_skeletons, u32 num_depth_frames, u32 num_ir_frames, u32 num_color_frames,
}
NuiSkeleton {
  u32 tracking_id; u8 seated; u8 up_status; u8 flags; u8 reserved;
  u16 reserved2; u32 reserved3;
  f32 position[3]; f32 orientation[4];          // world, metres
  f32 up_vector[3]; f32 up_confidence;
  f32 screen_position[2]; f32 screen_rect[4];
  NuiJoint joints[20];                           // Xbox 360 NUI joint ids/order
}
NuiJoint { f32 position[3]; f32 orientation[4]; f32 pos_confidence; f32 orient_confidence; }
```

The 20 joints are the standard Xbox 360 NUI set: Head, Neck, SpineChest, Pelvis,
L/R Shoulder, L/R Elbow, L/R Wrist, L/R Hand, L/R Hip, L/R Knee, L/R Ankle, L/R Foot.
Structs are packed and little-endian; the plugin byte-swaps into guest layout.

Optional per frame (only if a title needs depth; see §5): 320×240 u16 depth + intrinsics,
640×480 u8 IR, 640×480 BGRA colour, 4×24 kHz PCM16 mic array.

Confidence values are the single most important field: Kinect games gate on them, and
fabricating confident-but-wrong joints is worse than reporting low confidence.

### 3.3 Phone backend

* **`phone-embedded` (preferred).** Android app: CameraX + MediaPipe Tasks
  (`PoseLandmarker`, 33 BlazePose landmarks) running on-device, streaming **landmarks only**
  (<4 KB/frame) plus optional ToF depth (many Android phones have it) at 160×120.
  Latency 30–60 ms, one heavy model implementation. iOS/AVFoundation follow-up.
* **`phone-relay` (fallback).** Phone streams H.264/JPEG (+ToF depth) to the provider, which
  runs the model host-side. Simpler on the phone, 100–250 ms latency, more bandwidth.

Both speak the same wire format; `phone-embedded` emits `NuiFrame` directly, `phone-relay`
goes through the same pose front-end as `webcam`.

### 3.4 Hardware backend

* Use **libfreenect (v1)**, not libfreenect2 — libfreenect2 explicitly supports neither the
  Xbox 360 Kinect nor Kinect for Windows v1.
* Recommended sensor: **Kinect for Windows v1 (model 1517)**. Xbox 360 model 1414 needs the
  12 V power brick and, on Windows, a **destructive** Zadig WinUSB/libusbK driver swap that
  the user must perform deliberately; its colour camera is 320×240/best-effort and its depth
  encoding is the obscure one. Document, don't automate.
* Tilt motor (`SetTiltMotorAngle`) is implemented and exposed; it maps to the
  currently-stubbed `XamNuiCameraElevation*` family.
* Depth/IR from this backend bypass the pose core entirely — it is the quality path.

### 3.5 Pose core (shared by webcam/phone-relay)

* Front-end: MediaPipe Tasks `PoseLandmarker` (Apache-2.0) or ONNX Runtime + MoveNet.
  Model files cached locally, downloaded on first use, never bundled in the emulator repo.
* Mapper: 33 BlazePose landmarks → the 20 NUI joints above.
* Smoother: per-joint One Euro filter + confidence gating; **drop tracking on occlusion**
  rather than emitting a confident wrong pose.
* Scale: monocular depth is ambiguous. Calibrate from a 2 s T-pose + user-declared height
  (`--height-cm`), plus a torso-length-in-image heuristic, plus a per-title correction factor.
* Mirroring: Kinect is **not** mirrored by default; phone previews are. Default off.
* Latency compensation: optional one-frame velocity extrapolation (`--predict-ms`).

---

## 4. L2 — `kinect` plugin (homebrew guest XEX)

A small homebrew module loaded by the existing plugin loader:

```
storage_root()/plugins/<TITLE_ID>/
  plugins.toml          # enable flag + module hash, existing schema
  plugin.xex            # the module (built once with a 360 toolchain)
  nui/
    frame.0.bin         # double-buffered frame files from the provider
    frame.1.bin
    provider.json       # status/calibration mirror (optional)
```

**Responsibilities**

1. Dedicated guest thread at 30 Hz. Read the current frame file (open `frame.idx`, read
   `frame.N.bin`; provider flips by atomically replacing `frame.idx`). Skeletons are copied
   into a guest-side ring buffer, double-buffered, with the newest frame winning.
2. **Impersonate the Kinect device to xam**: `XMsgSystemProcessCall(0xFE, 0x2Bxxx, buf, len)`
   for camera messages and `0x2Cxxx` for identity, matching the ranges documented at
   `xam_app.cc:20-21`. Implement the callbacks that `xam_app.cc:165-176` leaves TODO
   (`0x2C009` Kinect identity callback is already registered by xam and currently ignored).
   **See §4a — this item is written but goes nowhere, and cannot be made to without
   patching xenia.**
3. `XNotifyBroadcast(kXNotificationSystemNUIHardwareStatusChanged /*0x00060019*/, state)` on
   connect/disconnect, and `kXNotificationSystemNUIPause /*0x0006001A*/` while a modal xam
   dialog is up — exactly the protocol described at `src/xenia/xbox.h:194-196`.
4. Serve the title's own consumption path when the title does not go through xam:
   locate the game's NUI entry point and feed it directly (via an L1 patch-installed
   trampoline or a runtime guest-memory detour from within the plugin).
5. Translate skeletons into whatever the title expects: units (metres vs. Kinect depth units),
   handedness, mirror, seated/standing, hand LOD/gesturing flags, tracking-id lifecycle.

**Dependencies / risks**

* Building a 360 homebrew XEX needs a 360 build environment (XDK `xbuild.py`, or an
  equivalent). This is the main practical blocker for L2 and must be resolved first; if it
  cannot be, fall back to §8.
* The plugin runs as guest code at the game's privilege level inside xenia's memory model —
  it is well isolated and hash-gated, but it is per-title work (one plugin per title id,
  sometimes per module hash).

---

## 4a. Finding: xam has no frame consumer, and "skeleton feed" was the wrong guess

Two corrections to the plan, both from measurement rather than reading the design doc.

### Correction 1: the plugin cannot feed xam

The plan assumed the plugin could impersonate the device to xam. It cannot, because
there is nothing on the other side to receive a frame.

**What the guest can do today.** The plugin calls `ExReadModifyWriteXConfigSettingUlong`
(xboxkrnl `0x2BC`) to set `X_RETAIL_FLAGS::KinectInitialized`. This is the same
operation as the console-settings checkbox, and it works from guest code:

```
set_flag=0                          # STATUS_SUCCESS
get_device_status=3221553158        # 0xC0050006 - no device
status_after=0                      # X_ERROR_SUCCESS - device present
```

This is the *only* part of the NUI surface the guest can change. xconfig lives on the
host (`xconfig_data_`, a C++ struct); `ReadSetting` is a `memcpy` out of host memory
(`xconfig.cc:46-57`), so no amount of guest RAM writing reaches it. An export is the
only door, and `0x2BC` is the one that writes.

**What xam has.** Nothing to feed. Every skeleton path is a stub that ignores its
arguments and returns a constant:

| Export | Ordinal | Implementation |
| --- | --- | --- |
| `XamNuiSkeletonGetBestSkeletonIndex` | `0x893` | returns `0xFFFFFFFFFFFFFFFF` |
| `XamNuiPlayerEngagementUpdate` | `0x892` | empty body, does nothing |
| `XamNuiGetDepthCalibration` | `0x860` | returns `STATUS_NO_SUCH_FILE` |
| `XUsbcamGetState` | `0x21D` | returns `CamNotConnected`, constant |

There is no skeleton buffer anywhere in the tree. `grep -ri skeleton src/xenia` matches
only the two export names — there is no storage, no ring buffer, no consumer. The
`0x2Bxxx` / `0x2Cxxx` message ranges that §4 item 2 proposes to impersonate are, in
`xam_app.cc:153-176`, a log line and a registered-but-ignored callback.

**Consequences.**

* L2 cannot be completed guest-side. Not "is hard" — cannot. There is no host state for
  the frame to land in. Closing this needs a patch to xenia that adds the consumer.
* This does not undermine the provider or the wire format. The provider is correct and
  the guest decodes its frames; that is the half that was in doubt and it is settled.
  What is missing is entirely on the emulator side.
* It does mean the spike should stop calling this "feeding xam". It sets the device
  flag and reads frames. Two real achievements, but the plan's framing overstated what
  was connected.

### Correction 2: skeletons are the wrong payload to optimise

The plan treated a skeleton feed as the goal. That was inferred from the two xam
ordinals that mention skeletons, which is the weakest possible evidence — it describes
what xenia *happens to name*, not what a Kinect *is*.

**On real hardware, depth is primary and skeleton is derived.** The NUI pipeline is
depth camera → depth image → skeletal tracking engine → skeleton frame. The engine
consumes depth to produce joints, so a device that emits only skeletons is emitting
the *output* of the part we would most want to reproduce.

This inverts the dependency, and it is the reason the plan was backwards. Emitting
skeletons means reimplementing the tracking engine's *result* and hoping the title
cannot tell. Emitting depth means the tracking problem stays where it belongs — and
titles that consume depth directly (per §5, e.g. *Kinect Star Wars*) work at all,
which a skeleton-only provider can never serve.

**The wire format already reflects this**, which is reassuring — it was designed
correctly and only the plan's framing was wrong. `NuiFrameHeader` carries
`num_depth_frames`, `num_ir_frames` and `num_color_frames` alongside `num_skeletons`
(`nui-provider/include/nui/frame.h:112-123`). The synthetic backend fills in only
`num_skeletons`; the other three are zero.

**What a real title imports — measured (2026-10-11, supersedes the "unmeasured" note)**

The obvious blocker was that the installed NUIView XEX is a `LIVE` container whose
embedded image is encrypted. But the *official Kinect dashboard* (`$flash_dash.xex`,
NXE Kinect flash) is not encrypted, and xenia dumps a module's import table at load
time (`user_module.cc:1199`, marking unimplemented ones `!!`). So this was never
unmeasurable — only NUIView was.

Measured on the dashboard's own import table:

| ordinal | import | xenia |
| --- | --- | --- |
| `0x21D` | `XUsbcamGetState` | implemented (stub returns a constant) |
| `0x222` | `XUsbcamCreate` | implemented (allocates `0x4B000` = 640×480) |
| `0x21E` | **`XUsbcamReadFrame`** | **STUB** |
| `0x21A`/`0x21B`/`0x21C`/`0x220`/`0x223` | `SetCaptureMode`/`GetConfig`/`SetConfig`/`SetView`/`Destroy` | STUB |
| `0x885` | `XamNuiGetDeviceStatus` | implemented (reads the retail flag) |
| `0x890` | `XamNuiIsDeviceReady` | implemented (the `>> 1` bug) |
| `0x892` | `XamNuiPlayerEngagementUpdate` | stub, empty body |
| `0x893` | `XamNuiSkeletonGetBestSkeletonIndex` | stub, returns `-1` |
| `0x894` | `XamNuiSkeletonScoreUpdate` | STUB |
| `0x47D` | `XamShowNuiHardwareRequiredUI` | implemented |
| `0x46B` | `XamShowNuiTroubleshooterUI` | implemented |

Two things fall out, and both matter more than the ordering argument did.

**The frame path is `XUsbcamReadFrame` (0x21E), not the skeleton exports.** The
dashboard imports the whole `XUsbcam*` family and treats it as a *camera device*:
`Create` → `SetCaptureMode` → `SetConfig` → `GetState` → `ReadFrame`. That is a
camera driver's contract, and it is where depth arrives. `XUsbcamCreate`'s existing
`0x4B000` buffer — exactly 640×480 — is the frame buffer. This confirms the
depth-primary conclusion by measurement rather than by argument, and it identifies
the specific export to implement.

**The dashboard statically links `XCAM`.** Its `XEX_HEADER_STATIC_LIBRARIES` list
includes `XCAM 2.0.20141.0` alongside XAPILIB/XBOXKRNL/XGRAPHC. That is a separate
camera library, and it means the 360's camera stack is its own component rather than
part of xam — which is consistent with `xam.xex` being a thin shim over it in xenia.

So the emulator-side work is concrete and bounded: implement the `XUsbcam*` device
(`Create`/`SetCaptureMode`/`SetConfig`/`GetState`/`ReadFrame`), feed it depth from the
provider, and only then worry about the skeleton exports — which on real hardware are
fed by the tracking engine sitting above this layer, not by the guest.

### The gate is a notification, not a return value (measured 2026-10-11)

Logging stubs added for the whole `XUsbcam*` family, then run against the dashboard.
They never fire. The dashboard's entire NUI call sequence is three calls:

```
XamNuiHudSetEngagedTrackingID(0)
XamNuiGetDeviceStatus(ptr)
XUsbcamGetState()          -> 0x00000003 (initialised + connected, with the flag on)
```

and then it registers `XamNotifyCreateListenerInternal(0x23, 1, 8)` and waits. It never
calls `XUsbcamCreate`, `SetConfig`, `SetCaptureMode` or `ReadFrame` — so no amount of
implementing the camera device will be reached, because the guest has not decided it
wants one yet.

**The dashboard is event-driven, not poll-driven.** It listens for
`kXNotificationSystemNUIHardwareStatusChanged` (`xbox.h:217`) and only probes the
camera once told a device appeared. Every earlier reading of this chain — including the
runbook's "the gate is `XUsbcamGetState`" — was inferred from the guest *spinning* on
that call, not from the guest *waiting* for an event it never receives.

So the next emulator-side step is not `XUsbcamReadFrame`. It is broadcasting
`kXNotificationSystemNUIHardwareStatusChanged` when the device state changes. The stubs
stay, because once that notification lands they are what reveal the capture mode and
frame layout.

### The real dashboard, measured (2026-10-11): the gate does not open

Ran the official Kinect dashboard (`$flash_dash.xex`, NXE Kinect flash) twice from a
clean store, once per flag state, 90 s each, and counted the calls:

| export | flag off | flag on |
| --- | --- | --- |
| `XUsbcamGetState` | 15774 | 16005 |
| `XamNuiIsDeviceReady` | 6027 | 6113 |
| `XamNuiGetDeviceStatus` | 2 | 2 |
| `XUsbcamCreate` | 0 | 0 |
| `XUsbcamReadFrame` | 0 | 0 |
| `XNotifyGetNext` (runtime) | 0 | 0 |

**The flag makes no difference, and the camera is never opened either way.** The
broadcast fires exactly once and the dashboard still does not reach `Create`.

Two things were verified as genuinely in place for the flag-on run, so this is not
another misconfigured run: `nui_fake_device = true` in the log, and
`X_RETAIL_FLAGS::KinectInitialized` set in the store's `xconfig.settings` — which is
what patch 0003's `XamNuiIsDeviceReady` needs to return non-zero. It returns 0
anyway, so the guest is not acting on that return, or something else gates it.

So the dashboard remains unsuitable as the driver for this work: it consumes the
camera only under conditions xenia does not yet reproduce, and it will not tell us
what `SetConfig`/`SetCaptureMode` want because it never calls them. The plugin-driven
probe (`smoke.ps1` and `ProbeCamera`) is the instrument that actually moves, and it
is the one to keep extending.

### NUIView, retried after patch 0002 (2026-10-11): blocked by its own UI, not by NUI

Retried NUIView (title `4D5308FB`) now that patch 0002 implements the three device
requests, which were **null imports** when this title was last measured. 90 s runs, with
and without `nui_fake_device`:

| export | flag off | flag on |
| --- | --- | --- |
| `PsCamDeviceRequest` | 2 | 2 |
| `McaDeviceRequest` | 0 | 0 |
| `DetroitDeviceRequest` | 0 | 0 |
| `XamNuiGetDeviceStatus` | 0 | 0 |
| `XamNuiIsDeviceReady` | 0 | 0 |
| `XUsbcamGetState` | 0 | 0 |

So patch 0002 did move something: `PsCamDeviceRequest` is now genuinely **called** (it was
an `undefined extern call` before, which is why the runbook recorded the app bug-checking
immediately afterwards). But it still calls no `XamNui*` export at all, and the flag makes
no difference.

**The blocker is upstream of NUI, and it is a UI asset failure.** NUIView's own
`default.xex` contains:

```
fontfile=file://game:\NUIView.xzp
scene=file://game:\NUIView.xzp
skin=file://game:\NUIView.xzp
```

and the run ends with the guest's own error string:

```
Error: Failed loading skin
```

The path resolution explains it exactly:

```
StfsContainerDevice::ResolvePath(default.xex)   <- works, container is fine
NullDevice::ResolvePath()  x6                    <- everything after it is empty
```

`GAME: => \Device\Content\1\` resolves to the null device, not to the STFS container that
holds `NUIView.xzp`. So `default.xex` loads, then every asset lookup behind it finds
nothing, the skin fails, and the app sits waiting on `XamInputGetKeystrokeEx` — an
interactive prompt, not a Kinect wait.

This is a content-mount problem, not a NUI problem, and it is a *different* problem from
the dashboard's. It would need the container's file index mounted at `\Device\Content\1\`
so `game:\NUIView.xzp` resolves. That is plausible to fix in xenia's content manager, but
it is a separate piece of work from anything in this document, and until it is done
NUIView cannot tell us anything about NUI — it never gets past its own first screen.

### The Gunstringer: a real Kinect title runs, and gets further than the dashboard

The user supplied a copy of **The Gunstringer** (Twisted Pixel, XBLA), a Kinect-*required*
Western puppet shooter — a genuine NUI consumer, and the first one on this machine.
Run from a clean store, 150 s, `nui_fake_device = true`.

**It runs.** Unencrypted `XEX2` `default.xex` (12.3 MB) alongside 3.8 GB of loose assets:
`.bik` video, `.xpr` textures, `.xwb` audio banks. The log shows it loading 1280x720
textures, decoding `Streaming.xwb` through `XmaContext` frame by frame, and drawing. This
is not a stalled boot — it is a game playing its intro.

`game:` **resolves correctly** here, unlike NUIView:

```
NtCreateFile(..., game:\intro1.ptc, ...)
NtCreateFile(..., game:\resident.xpr, ...)
NtCreateFile(..., game:\Streaming.xwb, ...)
```

So the NUIView content-mount problem does not apply to a title launched this way — worth
recording, because it means NUIView's failure was not a general `game:` defect.

**But it never reaches NUI.** Zero calls to any `XamNui*` or `XUsbcam*` export, with the
fake-device flag on or off. It is stuck well before that, in the intro:

```
NtQueryVolumeInformationFile(F80000B0, ...)  x34942
xeRtlNtStatusToDosError 103 => 3E5            # 0x103 = STATUS_PENDING, every time
```

A busy-poll on a volume query that never completes, then teardown of the handles and a
fresh load — repeating. That is an audio-streaming path (`Streaming.xwb` / `Commentary.xwb`)
blocking on a pending file query, which is an xenia file-system or audio issue, not a NUI
one.

Useful negative: the flag has no effect here either, because NUI is never consulted. And
the game is small and quick to load, which makes it a *better* probe than the dashboard
once this stall is cleared — no dashboard-scale boot, and a title that genuinely requires
Kinect rather than merely supporting it.

Also of note: the XBLA-installed `PIRS` container (`4D5309DF033F0001`, 6 MB) sits
alongside the loose assets and is not needed — `default.xex` plus the loose files is a
complete, directly loadable layout. That is the same trick NUIView's container needed and
did not get.

### The Gunstringer reaches the Kinect tuner — and answers the layout question

Manual GUI run of the same game (see above) got further than any headless run: the
window shows **the Kinect Tuner screen** — a tracked-silhouette preview top-right, a camera
box centre, "Grab me!" instruction. The game is sitting at its NUI setup stage, waiting.

Two things fall out of its XEX headers, both measured:

```
XEX_SYSTEM_SKELETAL_TRACKING_REQUIRED      <- skeletal tracking is mandatory
```

and a static-library list that names the real NUI stack:

```
XCAM     : 2.0.20764.0     the camera driver
NUI      : 2.0.20764.3
NUIHNDL  : 2.0.20764.0     NUI handles
NUISP    : 2.0.20764.3     NUI Simple Player - the device layer
NUIAUD   : 2.0.20764.0     NUI audio
```

So the 360 NUI stack is **five** static libraries, not one, and `NUISP` is the player-side
device layer — which is why the `XamNui*` exports are a thin management surface rather
than the data path.

**The import table settles the frame-layout question** that has been open since the
dashboard measurement. This title imports the whole `XUsbcam*` family:

| ordinal | import | xenia |
| --- | --- | --- |
| `0x222` | `XUsbcamCreate` | impl (allocates `0x4B000`) |
| `0x21A` | `XUsbcamSetCaptureMode` | impl (our logging stub) |
| `0x21C` | `XUsbcamSetConfig` | impl (our logging stub) |
| `0x220` | `XUsbcamSetView` | impl (our logging stub) |
| `0x21D` | `XUsbcamGetState` | impl |
| `0x21E` | **`XUsbcamReadFrame`** | impl (our stub, returns NOT_IMPLEMENTED) |
| `0x223` | `XUsbcamDestroy` | impl (our logging stub) |
| `0x30D` | `PsCamDeviceRequest` | impl (patch 0002) |

plus the tilt/elevation group (`0x880`, `0x883`, `0x865`, `0x86D`, `0x895`) and the identity
group (`0x873`, `0x86A`, `0x863`). **The camera-driver contract is confirmed by a real
consumer** — independently of the dashboard, which imports the same family but never calls
it.

It has still made zero runtime NUI calls, so the stubs are not yet logging what it passes.
What is now known: it *wants* exactly the family we instrumented, and the stubs will answer
the moment it reaches them. That is a much stronger position than the dashboard gave us.

### Kinect Adventures: first real NUI call from any title, and a user-account blocker

User-supplied `kinect_adventures.iso` (7.8 GB DVD image). **xenia mounts an ISO directly**
— `xenia_main.cc:120` documents "the target .xex or .iso to execute", and `DiscImageDevice`
is a proper VFS device (`vfs/devices/disc_image_device.cc`). No extraction step is needed:

```
DiscImageDevice::Initialize
Loading module GAME:\default.xex
```

Microsoft's own first-party Kinect title, and it behaves differently from both previous
candidates. It got further:

```
XamNuiGetDeviceStatus      1
XamUserGetDeviceContext    64788     <- the spin
```

**That single `XamNuiGetDeviceStatus` is the first NUI call any real title has made** — the
dashboard never got there, and The Gunstringer called nothing. Its call sequence so far:

```
XamNuiGetDeviceStatus(ptr)
... then XamUserGetDeviceContext for users 0, 1, 2 ...
```

**The blocker is an absent user account, not the camera.** `XamUserGetDeviceContext` is
spinning for all three user indices with a null result. `XamUserGetXUID` was called 13 times
and `XamGetStagingMode` never — the title is past staging and into a signed-in flow it
cannot complete. Both Kinect conditions were satisfied during the run
(`KinectInitialized` set in the store's `xconfig.settings`, `nui_fake_device = true`), so
this is not another flag-configuration miss.

So the title is waiting on a logged-in profile before it will touch NUI. That is testable —
xenia supports creating profiles — and unlike the previous two candidates this is a known
xenia capability rather than an unknown one.

### `--nui_fake_device` was never actually set

Every run so far passed `--nui_fake_device` on the command line and the log printed
`nui_fake_device = false`. xenia only parses command-line cvar overrides on Android
(`base/cvar_android.cc`; the Windows build reads them from
`storage_root/xenia-canary.config.toml`). Every prior "the flag works" conclusion from
this host was therefore not testing what it claimed to be.

Correct invocation, verified working:

```toml
# <store>/xenia-canary.config.toml, under [Kernel]
nui_fake_device = true
```

With it genuinely on, `XUsbcamGetState` returns `0x00000003` and the dashboard advances
to the notify-listener stage. That is real progress, and it is *different* progress from
what was previously reported — it reaches the listener rather than stopping at the
camera probe.

**Revised order.** Depth first, in the provider and on the wire — it is the primitive
the hardware produces and the one skeletons derive from. Then the xenia-side consumer.
Then the guest plugin becomes a thin shim over an interface that already exists.
Doing the guest side first, as planned, optimised the part that already worked, over a
payload chosen by guesswork.

---

## 5. L1 — patches (data-only, per title)

`storage_root()/patches/<TITLE_ID>*.patch.toml`, existing schema (`patch_db.h`). Because
patches are raw byte writes at guest addresses, they can install small PPC stubs as well as
plain constants — everything here stays within that capability:

* Bypass/neutralize "no Kinect connected" failure branches in the title.
* Install a trampoline that redirects the title's NUI call site to the plugin's exported
  function (bytes + a resolved target address supplied by the plugin at runtime).
* Embed per-title calibration constants (height, mirror, coordinate basis) directly into
  guest memory.
* Force no-op / auto behaviour on the camera tilt and elevation calls.
* Skip the game's own "device is warming up" retries.

Patches are title-id + module-hash gated, which is exactly the right blast radius: a patch
that breaks one build cannot affect another.

### Depth question (settled in Phase 0)

Games such as *Kinect Star Wars* consume depth directly rather than skeletons. Two answers:

1. **Preferred:** feed synthetic depth from the provider (phone ToF, or Kinect hardware)
   through the plugin. No patches needed beyond the title's normal NUI setup.
2. **Fallback:** a title patch that redirects the title's depth buffer to a
   skeleton-derived pseudo-depth plane.

Phase 0 determines which titles need which.

---

## 6. Phase 0 — decisive spike (3–5 days, before anything else)

The architecture above assumes the emulator need only supply the Kinect pipeline's *output*
(skeletons/images), not raw sensor frames. Verify that, and verify the plumbing, with a
throwaway instrumented build — do **not** start the provider app until this passes.

Tooling lives in [`tools/nui-spike/`](../tools/nui-spike/README.md): the instrumentation is
a single git patch, the analysis is a Python script, and a podman container provides the
whole build so the host needs no toolchain. The patch is throwaway — apply it, measure,
revert — and it compiles and links clean (verified: full `xenia-app` builds and runs).

```sh
git apply tools/nui-spike/0001-nui-trace-instrumentation.patch   # throwaway
./tools/nui-spike/podman-check.sh xenia-app                      # build in the container
./tools/nui-spike/extract-binary.sh                             # -> ../xenia-nui-build/
../xenia-nui-build/run.sh -log_nui_trace 0x1F                    # run it
```

Then measure. For each target (dashboard, then each NUI title), take two runs —
**Kinect Initialized off** and **on** (console settings → Retail Options):

```sh
./xenia-canary-canary.xenia -log_nui_trace 0x1F     # exports+messages+notify+hexdump
mv <storage>/nui_trace/trace.tsv trace-off.tsv
# toggle the checkbox, relaunch, play the interesting bit
mv <storage>/nui_trace/trace.tsv trace-on.tsv

python3 tools/nui-spike/analyze.py trace-on.tsv --csv surface.csv
python3 tools/nui-spike/analyze.py trace-off.tsv trace-on.tsv --diff
```

Spike tasks:

0. **Get a guest module that talks NUI.** *NUIView* — the Xbox 360 Kinect developer test
   application, title `4D5308FB`, ~43 MB, NUI-only — is the ideal stand-in for a retail
   title, and xenia launches it directly. If you have a copy, `./tools/nui-spike/trace-nuiview.sh on|off`
   runs it and summarises the trace. Otherwise the route is a ~200-line `nui-probe.xex`
   written with the open Xenon toolchain
   ([josevolpato/xenon-toolchain](https://github.com/josevolpato/xenon-toolchain), GPL-3.0,
   XDK-free) plus [libxenon](https://github.com/xenia-project/libxenon): call
   `XamNuiGetDeviceStatus`, `XamNuiIsDeviceReady`, `XamNuiPlayerEngagementUpdate`,
   `XMsgSystemProcessCall(0xFE, 0x2B003, …)` and `XNotifyBroadcast`, which exercises both
   the export surface and the message path. Open question to resolve first: whether
   libxenon's import stub covers `xam.xex` or only `xboxkrnl`.
1. Enable `allow_plugins`, put the probe in `storage_root()/plugins/<TID>/`, confirm it loads
   and can call `XMsgSystemProcessCall(0xFE, ...)` and `XNotifyBroadcast`, and can read/write
   a file under `plugins:`. **This validates the whole L2 story.** (Note the plugin loader is
   title-id + module-hash gated, so a standalone `.xex` loaded from the command line has
   `title_id = 0` and will *not* pick up plugins — see `emulator.cc:1636-1645`. For the
   plugin path a real title is needed; for the export/message path the probe is enough.)
2. Read the `STATE retail_flags` line to confirm the checkbox is what flips the guest's
   belief, and check whether `XamNuiIsDeviceReady` returns 0 even when it is set (§1).
3. From the `--diff`, produce the **required surface list**: which exports a title actually
   needs and with what semantics. This is the spec for the provider's output and the plugin's
   job. `--hexdump 0x2Bxxx` decodes the message buffers.
4. Answer the depth question (§5) per title: does it request depth/IR, or only skeletons?
5. Watch for any guest-side USB access by the NUI component — if frames are only reachable
   through a bus we do not emulate, scope that title out explicitly.

Until step 0 exists, the spike still yields the boot state (`STATE` lines) and the static
surface table from `gen_surface.py`, which is already written up in §1.

**Exit criteria:** plugin loads and delivers a message to xam; a title is observed calling
the NUI surface with "Kinect Initialized" set; surface list written down.

**Outcome (measured on Windows against the NXE Kinect flash — see runbook §7–§8):**

* ✅ A title *was* observed calling the NUI surface with the flag set. Surface list
  written down: the dashboard's entire NUI surface is five exports —
  `XamNuiGetDeviceStatus`, `XamNuiHudSetEngagedTrackingID`, `XamIsNuiAutomationEnabled`,
  `XamNuiIsDeviceReady`, `XUsbcamGetState`.
* ✅ The gate chain is fully resolved and ordered: Kinect flag → `XamNuiGetDeviceStatus`
  ✅; `--nui_fake_device` → `XUsbcamGetState` ✅; `XamNuiIsDeviceReady` ❌ **always 0**.
* ❌ **Plugin loading was never tested** — it needs a real title hash and a 360 homebrew
  toolchain, and the dashboard never got far enough to be worth it.
* ➡️ The blocker is host-side, so the L2 plugin story is untested but also not currently
  on the critical path. Fix `XamNuiIsDeviceReady` first, re-measure, and only then decide
  whether a homebrew plugin is needed at all.

Revert when done:

```sh
git apply -R tools/nui-spike/0001-nui-trace-instrumentation.patch
```

---

## 7. Phases

| Phase | Deliverable | Depends on |
|---|---|---|
| 0 | Spike + surface spec (§6) — **done**; gate chain resolved, §8 fix identified | — |
| 0.5 | **Fix `XamNuiIsDeviceReady` (§8 row 2)** — **done**, and verified sufficient | 0 |
| 1 | `nui-provider` with `synthetic` + `replay` backends; IPC writer; frame format frozen; Catch2 tests for codec/smoother/mapper | 0.5 |
| 2 | Hello-world plugin → frame-consuming plugin; `XNotifyBroadcast` handshake; skeleton ring buffer | 1 |
| 3 | Per-title patches: skip connection checks, redirect NUI calls | 2 |
| 4 | `webcam` backend + pose core (first *usable* end-to-end setup) | 3 |
| 5 | `phone` backend (embedded first, then relay) + ToF depth path | 4 |
| 6 | `hardware` backend (libfreenect vendored in the provider repo, not xenia) + tilt | 4 |
| 7 | Identity/HUD paths that need xam cooperation → §8 | as needed |
| 8 | Docs: per-title setup guide, hardware notes, troubleshooting | all |

---

## 8. The fallback PR (only what cannot be done from guest code)

If Phase 0 or the toolchain constraint forces in-tree work, keep it to a single, isolated,
default-off PR — no cvars renumbered, no behaviour changes when disabled:

| Change | Size | Why it may be unavoidable |
|---|---|---|
| `NuiFrame` ingestion on the xam side: a small reader that polls the provider's frame file and fills a guest-visible skeleton buffer | ~300 lines, 4–5 new files | lets titles consume skeletons without a homebrew plugin |
| Fix `XamNuiIsDeviceReady` (`xam_nui.cc:185-201`) to report ready when a NUI provider is present | ~10 lines | guest code cannot fix a host-side export |
| Implement `XamNuiIdentity*` stubs as plausible successes (`xam_nui.cc:313-352`) | ~40 lines | enrollment is gated on host state |
| Broadcast `kXNotificationSystemNUIHardwareStatusChanged` on device state change | ~20 lines | matches the contract documented at `xbox.h:194-196` |

All four are additive, behind one `DEFINE_string(nui_frame_source, "none", ...)`-style cvar
in a new `src/xenia/kernel/xam/nui/` directory. Default `none` ⇒ zero behaviour change and
a near-zero merge burden against upstream. This is the *only* place in this plan where fork
divergence is proposed.

**Phase 0 outcome — row 2 is no longer conditional.** The `XamNuiIsDeviceReady` fix is now
*measured* to be required, and the other three remain plausible-but-unproven. So start with
row 2 on its own: it is roughly one line, it has no cvar, and it is an unambiguous upstream
bug (`return kinect_initialized >> 1 & 1;` on a `bool`). Land and re-measure before
building any of the rest; the surface after that gate is still unknown, and it may turn out
that a title which only probes the device — like the dashboard — needs nothing more.

---

## 9. Validation

Titles in increasing difficulty:

1. Xbox 360 Kinect Dashboard — enrollment/sign-in flow (exercises §8 rows 3–4)
2. Kinect Sports / Kinect Adventures — skeleton quality and gesture latency
3. Joy Ride — sustained tracking, fast motion
4. Kinect Star Wars — **depth** consumer (§5)
5. Kinect Hub / Disneyland Adventures / Robot City — colour stream and avatars

Quantitative bars (per title, tracked in the provider's stats output):

* tracking-id present on ≥ 98 % of frames while the player is in frame
* end-to-end latency < 80 ms (hardware, webcam) / < 120 ms (phone)
* per-jitter under the threshold at which the title's own gesture recogniser stops firing
* no xenia crash, hang, or unbounded guest-memory growth over a 30 min session

---

## 10. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| No 360 toolchain available for L2 | High | Resolve in Phase 0; otherwise §8 PR (built with normal host toolchains) delivers the same user-visible feature |
| A title performs its own skeleton processing on raw frames | Critical | Phase 0 diff; if true, redirect via patch (§5) or scope the title out explicitly |
| `XamNuiIsDeviceReady` always false blocks enrollment | Medium | §8 (small fix) or title patch; documented at §1 |
| Monocular-RGB pose is not KinectFusion-stable | High | `webcam` before `phone`; ToF depth path; confidence gating; document honestly that depth-capable sources are much better |
| Per-title breakage | Medium | hash-gated patches; one plugin per title id; regression list in docs |
| Guest module = user code running inside the emulator | Low | hash-gated, opt-in cvar, isolated in storage root |
| Licensing | Low | libfreenect Apache path, MediaPipe Apache-2.0, no firmware redistribution |
| Upstream removed Kinect support once already | Low | design keeps all of it outside the fork |

---

## 11. Repository layout (provider, separate repo)

```
nui-provider/
  include/nui/frame.h          # wire format (frozen, shared with plugin)
  src/backends/{synthetic,webcam,phone,hardware,replay}/
  src/pose/{frontend,mapper,smoother,calibration}/
  src/ipc/{file,udp}.cpp
  tests/                       # Catch2: codec, mapper, smoother
  tools/                       # provider stats viewer, frame inspector
```