# Kinect spike runbook — Windows host

For running the instrumented xenia build on a **real Windows machine**. Everything here
was derived from the Phase 0 spike; see [`kinect_support.md`](kinect_support.md) for the
design it feeds.

- Test title: **NUIView**, the Xbox 360 Kinect developer test application
  (title `4D5308FB`, ~43 MB, NUI-only). It is the cheapest thing that exercises the NUI
  path; a retail NUI game is not needed yet.
- Binary: built by `.github/workflows/NuiSpikeWindows.yml` on a GitHub runner, with two
  patches applied at build time:
  - `tools/nui-spike/0001-nui-trace-instrumentation.patch` — the tracer (throwaway)
  - `tools/nui-spike/0002-nui-device-request-stubs.patch` — `PsCam`/`Mca`/`Detroit`
    device requests and the `XUsbcamGetState` camera state, all behind the
    `--nui_fake_device` knob

---

## 1. Get the binary

On the machine that has the repo:

```sh
./tools/nui-spike/push-and-build.sh <your-github-user>    # if not pushed yet
gh run list --repo <your-github-user>/xenia-canary --limit 3
gh run download <run-id> --repo <your-github-user>/xenia-canary \
   --name xenia_canary_nui
```

You get `spike/xenia_canary.exe` plus `analyze.py`, `gen_surface.py`,
`set-kinect-flag.py`, `nui_surface.csv` and this directory's `README.md`.

## 2. Lay out the content

xenia's XBLA layout is `<content_root>/0000000000000000/<TITLE_ID>/<MEDIA_ID>/<MODULE_HASH>`.

```
<store>\
  xenia_canary.exe
  content\0000000000000000\4D5308FB\00080000\3A219CD8ADA8B57F244A06C8EF49B9A4E3191EF94D
  test.xex                       (optional: the 4 KB hand-made homebrew)
```

`content\` under the storage root is the default content location, so no
`--content_root` is needed.

NUIView's XEX carries its own assets; xenia extracts them on load
(`default.xex`, `NUIView.cfg`, `NUIView.xzp`, `database.gmsodf`,
`MicArrayCalibrate.wav`, `NuiIdentityNN.bin.be`, `NuiIdentityPCA.bin.be`,
`pointsprite.png`).

## 3. Turn the Kinect "on"

Without a Kinect, xenia advertises nothing — `XamNuiGetDeviceStatus` fails, and a title
bails out before it ever touches the NUI surface. The console-settings checkbox
(Tools → Console Settings → Retail Options → **Kinect Initialized**) sets this; from a
script, patch the xconfig blob instead (verified: offset 2334, big-endian):

```sh
python set-kinect-flag.py <store>\xconfig.settings show
python set-kinect-flag.py <store>\xconfig.settings on
```

The flag must exist before launch; xenia creates `xconfig.settings` on first run, so
either tick the checkbox once in the UI or run once to create the file and then patch it.

## 4. Run

```bat
xenia_canary.exe --enable_console --log_level 3 --log_nui_trace 0x0F ^
  --storage_root "C:\nui-spike" ^
  "C:\nui-spike\content\0000000000000000\4D5308FB\00080000\3A219CD8ADA8B57F244A06C8EF49B9A4E3191EF94D"
```

| flag | why |
|---|---|
| `--enable_console` | the exe is GUI-subsystem; `AllocConsole()` gives you a window (`console_win.cc:38-48`). It may open behind the emulator — alt-tab. |
| `--log_file <path>` | writes every line as it happens, so the tail survives a crash. **Always use this.** |
| `--log_nui_trace 0x0F` | exports + messages + notify. `0x1F` adds `0x10` = trace *everything*; see §6. |
| `--nui_fake_device` | optional: report the NUI device as present (`0002`). |

Output:

- `C:\nui-spike\xenia.log` — full log
- `C:\nui-spike\nui_trace\trace.tsv` — the trace

## 5. Analyse

```sh
python analyze.py C:/nui-spike/nui_trace/trace.tsv
python analyze.py C:/nui-spike/nui_trace/trace.tsv --csv surface.csv
python analyze.py off.tsv on.tsv --diff
python analyze.py trace.tsv --hexdump 0x2B003
```

`--csv` is the deliverable: the list of APIs a title touches, with call counts.

---

## 6. Traps

**Every flag needs `--`.** A single-dash flag is parsed as a group of short options,
cxxopts throws, and xenia shows a message box and calls `exit(0)` — the emulator silently
never starts, *with exit code 0*. The same thing happens when the flag is valid but the
value is missing (`--nui_fake_device true` treats `true` as the title path). If the
process vanishes instantly with nothing on disk, suspect this before anything else.
Validate with `--help` alone.

**Junk in the working directory** (`.xdp-*`, `xenia.log`, extracted content) is the
fingerprint of a rejected `--storage_root`: xenia falls back to the cwd.

**The default trace filter misses the interesting call.** It matches
`Nui`/`Natal`/`Kinect`/`ControlPack`/`Biometric` — **not** `PsCamDeviceRequest`, which is
the exact call NUIView makes to decide whether a camera exists. Use `--log_nui_trace 0x1F`
to see everything, or pass a filter explicitly:

```bat
--nui_trace_filter PsCam,Mca,Detroit,Nui,Natal,Kinect,ControlPack
```

**`log_nui_trace` is persisted** into `xenia-canary.config.toml` on exit, so pass it on
every run rather than trusting the stored value.

**A trace with only `STATE` lines means the xam module never loaded** — i.e. the title did
not start, not that tracing is broken.

---

## 7. What we already know (so you don't re-measure it)

### The dashboard's NUI init chain (measured with the official Kinect dashboard)

The official Kinect dashboard (`$flash_dash.xex`, title id **`FFFE07D1`** = xenia's
`kDashboardID`, so `IsSystemTitle()` is true and `$flash_xam.xex` loads correctly) is a far
better probe than NUIView. Running it under Proton with the tracer produced this chain:

```
XamNuiGetDeviceStatus   -> 0xC0050006   (no Kinect; X_CONFIG flag not set)
   ... with the Kinect flag set ...
XamNuiGetDeviceStatus   -> 0x00000000   (success)
XUsbcamGetState (0x21D) -> 0            (CamNotConnected - hardcoded)
   -> dashboard gives up here; black screen
```

So a black screen has (at least) two independent causes, both fixable:

1. **`Kinect Initialized` not set in the storage root you launch with.** Each storage root
   has its own `xconfig.settings`; a fresh one has the flag clear, so
   `XamNuiGetDeviceStatus` fails and no title ever reaches the NUI surface. Check with
   `python set-kinect-flag.py <store>\xconfig.settings show`.
2. **`XUsbcamGetState` hardcodes "not connected"** (`xboxkrnl_usbcam.cc:38-43`). The
   dashboard reads it immediately after a successful device-status call and stops.
   Patch `0002` now makes it report *initialised + connected* under `--nui_fake_device`.

That is the whole answer to "why is it a black screen": the guest is not broken and the
content is not missing — it is correctly detecting an absent camera, three calls deep.

### The next gate, and a sanity check you need

With all three patched (`--nui_fake_device` + the Kinect flag), the dashboard's NUI calls
are only these, in this order:

```
XamNuiHudSetEngagedTrackingID   x2
XamNuiGetDeviceStatus           x2   -> 0x00000000 (success)
XUsbcamGetState                 x2   -> 0x00000003 (initialised + connected)
ExGetXConfigSetting(7, 0x04)          (XCONFIG_CONSOLE_CAMERA_SETTINGS)
ExSetXConfigSetting(7, 0x04)          (records the camera state; xenia honours it)
```

It then stops calling anything NUI-related. That is where the next gate is — but on
Linux/Proton you cannot see it, because:

> **No swapchain was ever created.** `VulkanPresenter: Created 1280x720 swapchain ...`
> is absent from every Proton log here. The emulator window never presents anything, so
> "black screen" there is a *presentation* failure, not a Kinect one.

**Sanity check on your Windows host, before reading anything into the screen:** confirm
your log contains `Created ... swapchain`. If it does not, a black screen tells you
nothing about NUI. If it does, and the Kinect dialogs still do not appear, that is the
real result.

Other unimplemented exports the dashboard needs (they appear as `undefined extern call`):
`EtxProducerRegister`, `XdfInitialize`, `VdDisplayFatalError`. None NUI-specific, all
worth adding to the not-declared list if a title needs them.

### NUIView

| Observation | Detail |
|---|---|
| NUIView imports **35** unimplemented exports | Three were NUI-critical and fatal: `PsCamDeviceRequest` (0x30D), `McaDeviceRequest` (0x30E), `DetroitDeviceRequest` (0x30F). In `xboxkrnl_table.inc:795-797` with no implementation → null import → `undefined extern call`. |
| The app calls `PsCamDeviceRequest` at **two** sites | One 4-argument and one 3-argument call. |
| Our stub's reply decides everything | `0xC0000001` ("device absent") → NUIView reports no camera and idles in a `KeTlsGetValue` loop. It calls **no** `XamNui*` export at all. |
| Its whole NUI surface is 5 functions | `XamNuiIdentityGetSessionId` (implemented), `XamUserNuiGetUserIndex` (stub), and the three device requests. Get the list without running anything: xenia dumps a module's import table on load, marking unimplemented ones with `!!` (`user_module.cc:1199`). |
| `XamNuiIsDeviceReady` is broken | Returns `kinect_initialized >> 1 & 1` = always 0 (`xam_nui.cc:185-201`). |

### Measured on Windows: the Kinect flag alone is not enough

Run on a real Windows host against the **NXE Kinect flash** (`$flash_dash.xex`, title
`FFFE07D1`), stock `xenia_canary.exe`, `--log_level 3`. Note that `--log_level 3` already
prints every export call (`d> ... Name(...)`), so this A/B needs **no instrumentation
patch at all** — it is reproducible with any xenia build.

```bat
xenia_canary.exe --enable_console --log_level 3 ^
  --log_file <store>\run.log --storage_root <store> ^
  "<store>\NXE Kinect\$flash_dash.xex"
```

| call | Kinect flag **off** | Kinect flag **on** |
|---|---|---|
| `XamNuiGetDeviceStatus` | 7 | 3 |
| `XamNuiHudSetEngagedTrackingID` | 2 | 2 |
| `XamNuiIsDeviceReady` | 794 | 1324+ |
| `XUsbcamGetState` | 2499 | 3622+ |

(`+` = still climbing when the run was stopped.)

Three things fall out of this:

1. **The flag does what it claims.** `XamNuiGetDeviceStatus` drops from 7 calls to 3 —
   it stops failing/retrying and succeeds immediately. The checkbox really is what flips
   the guest's belief.
2. **But the dashboard does not advance.** With the flag on it *still* spins on
   `XUsbcamGetState` and `XamNuiIsDeviceReady`, and after ~2.5 minutes it had reached
   **no further NUI export at all** — the full set ever called is those four names.
   So the gate is not the Kinect flag.
3. **The gate is `XUsbcamGetState`**, which hardcodes "not connected"
   (`xboxkrnl_usbcam.cc:38-43`). That is exactly what patch 0002's `--nui_fake_device`
   flips to `initialised + connected`. So the previous run's observation is confirmed
   on Windows: **the Kinect flag and the fake camera are two separate gates, and you
   need both.**

Practical consequence: `--nui_fake_device` is not optional instrumentation for this
title — without it there is nothing further to measure, because the guest never gets
past the camera probe. That makes the instrumented CI artifact a hard prerequisite for
the remaining half of §8.

Also worth recording: the dashboard is not "black-screening" on Windows, it is
*busy-waiting*. The symptom to expect while it is blocked is pegged CPU and a
`XUsbcamGetState` loop in the log, not a hang.

## 8. The open question, and how to answer it

With `--nui_fake_device`, `PsCam`/`Mca` report success, `Detroit` reports a tilt controller
and `XUsbcamGetState` reports a connected camera. What the dashboard does *next* is the
measurement that matters — the exports after `XUsbcamGetState` are the surface the
Kinect provider must eventually satisfy.

Two runs to make it deterministic:

1. ~~**baseline** — Kinect flag set, *no* `--nui_fake_device`, mask `0x1F`. Expect it to
   stop at `XUsbcamGetState`.~~ **Done** (see "Measured on Windows" above): it never
   gets past `XUsbcamGetState` and spins there indefinitely.
2. **fake device** — same plus `--nui_fake_device`. Diff the traces: every new call is
   part of the "Kinect present" path. **This is the one still outstanding**, and it
   *requires* the instrumented build, because `nui_fake_device` only exists once patch
   0002 is applied.

So the remaining measurement needs `.github/workflows/NuiSpikeWindows.yml` to be
dispatched and the artifact downloaded. Everything else for it is already in place: both
patches apply cleanly to this branch, and the Kinect flash content is on disk.

On Linux-under-Proton the faked run exited early rather than continuing; a real Windows
host is the cleaner environment for this.

Please attach, for whichever run fails:

- `xenia.log` (the tail is enough, but the whole file is fine)
- `trace.tsv`
- the exact command line

That is the last input needed to turn [`kinect_support.md`](kinect_support.md) into an
implementation spec — specifically the provider's output contract and the plugin's job.