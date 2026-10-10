# Kinect (NUI) support — design plan

**Status:** design / pre-implementation
**Constraint:** keep the fork as close to upstream `xenia-canary` as possible. Everything
that can live outside the emulator tree *does*.

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

### Known gap found during review

`XamNuiIsDeviceReady` (`xam_nui.cc:185-201`) computes `kinect_initialized >> 1 & 1`, which is
`0` for `0x20000000` — i.e. it always reports *not ready*, even with the checkbox on.
That makes `XamNuiHudIsEnabled` (`:272-285`) false and `XamNuiIdentityEnrollForSignIn`
(`:327-341`) fail. So:

* "Kinect present" is already obtainable with zero code, but
* "Kinect ready / identity" is not, and must be handled by patch, plugin, or the fallback PR (§8).

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

Revert when done:

```sh
git apply -R tools/nui-spike/0001-nui-trace-instrumentation.patch
```

---

## 7. Phases

| Phase | Deliverable | Depends on |
|---|---|---|
| 0 | Spike + surface spec (§6) | — |
| 1 | `nui-provider` with `synthetic` + `replay` backends; IPC writer; frame format frozen; Catch2 tests for codec/smoother/mapper | 0 |
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
in a new `src/xenia/kernel/xam/nui/` directory. Default `none` ⇒ zero behaviour change and a
near-zero merge burden against upstream. This is the *only* place in this plan where fork
divergence is proposed.

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