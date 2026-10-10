# NUI spike tooling (Phase 0)

Throwaway tooling for the Kinect/NUI design spike described in
[`docs/kinect_support.md`](../../docs/kinect_support.md) §6.

Goal: find out **exactly which NUI surface a title touches** — which exports, with
which arguments, which return values, which 0x2B/0x2C messages — so the out-of-tree
Kinect plugin can be specified against measurements instead of guesses.

Nothing here is meant to be merged. The instrumentation is delivered as a git patch,
applied only while measuring.

```
tools/nui-spike/
  0001-nui-trace-instrumentation.patch  the instrumentation (apply/revert)
  analyze.py                            trace summariser / run differ
  gen_surface.py                        NUI export surface table from the sources
  nui_surface.csv                       generated surface table (129 exports)
  Containerfile                         podman build env (clang 19, gtk, ninja)
  podman-check.sh                       host wrapper: build image + compile/build
  check-build.sh                        in-container: mirror repo, submodules, ninja
  extract-binary.sh                     copy the emulator out of the volume
  run-container.sh                      run the built emulator inside the container
  teardown.sh                           remove image + volume + containers
  testdata/sample.tsv                   fixture for analyze.py
```

## 1. Apply the instrumentation

```sh
cd /path/to/xenia-canary
git apply tools/nui-spike/0001-nui-trace-instrumentation.patch
```

It adds `src/xenia/kernel/nui_trace.{h,cc}` plus four one-line hook sites
(`kernel/util/shim_utils.h` export trampolines, `xam/apps/xam_app.cc` message
dispatch + constructor, `xam/xam_notify.cc` `XNotifyBroadcast`). With no cvar set it
is a no-op.

Build normally (`xb build` / `ninja -C build`), then revert when done:

```sh
git apply -R tools/nui-spike/0001-nui-trace-instrumentation.patch
```

## 2. Run it

### Wayland sessions need `GDK_BACKEND=x11`

This is the one thing that will otherwise waste your time. On a Wayland session the
Vulkan presenter cannot get a surface: it asks for `Surface::kTypeFlag_XcbWindow`,
`GDK_IS_X11_DISPLAY` is false, `GTKWindow::CreateSurface` bails out
(`src/xenia/ui/window_gtk.cc:280-293`) and the emulator **aborts** shortly after,
usually right at `Loading module ...`:

```
!> GTKWindow: The window system of the GTK window is not supported by Xenia
```

XWayland makes this look like it should work; it does not. Force GDK onto X11:

```sh
GDK_BACKEND=x11 ../xenia-nui-build/run.sh --log_nui_trace 0x1F ...
```

`trace-nuiview.sh` does this for you. With it the swapchain comes up
(`VulkanPresenter: Created 1280x720 swapchain ...` through `VK_KHR_xcb_surface`) and
titles load and run.

### Arguments use the `--long` form

`xenia_canary --log_nui_trace 0x1F`, not `-log_nui_trace`. Single-dash flags are
parsed as short options, giving `Option 's' does not exist` and the usage banner.

### Tracing flags

| `log_nui_trace` | effect |
|---|---|
| `0x1` | NUI-ish exports (`Nui*`, `Natal*`, `ControlPack*`, `Kinect*`, `Usbcam*`, `Biometric*`) |
| `0x2` | all xam app messages (0x2Bxxx camera / 0x2Cxxx identity included) |
| `0x4` | `XNotifyBroadcast` |
| `0x8` | hexdump message buffers (adds volume; 256 bytes per message) |
| `0x10` | every export, not just NUI-ish ones |

**`0x1F` includes `0x10`**, so it traces *everything* and is far noisier than you
probably want — use `0xF` for the NUI-only view. Narrow further with
`--nui_trace_filter XamNuiSkeleton,XamNuiPlayerEngagement`.

The cvar is persisted: xenia writes `log_nui_trace = 0` into
`xenia-canary.config.toml` on exit, so pass the flag on every run.

Then do the thing you are measuring:

1. Dashboard, **Kinect Initialized off** → produces `<storage>/nui_trace/trace-off.tsv`
2. Dashboard, **Kinect Initialized on** (console settings → Retail Options) → `trace-on.tsv`
3. Repeat for each NUI title you care about → `trace-<titleid>-off/on.tsv`

`<storage>` is the xenia storage root, e.g. `%LOCALAPPDATA%\Xenia` on Windows,
`~/.local/share/xenia` on Linux. Rename the file between runs; it is truncated on
each start.

Keep runs short (30–60 s of the relevant interaction) — `0x10` traces produce very
large files.

## 3. Read the trace

```sh
python3 tools/nui-spike/analyze.py trace.tsv
python3 tools/nui-spike/analyze.py trace.tsv --csv surface.csv
python3 tools/nui-spike/analyze.py trace-kinect-off.tsv trace-kinect-on.tsv --diff
python3 tools/nui-spike/analyze.py trace.tsv --hexdump 0x2B003
```

`surface.csv` is the deliverable of Phase 0 step 3: the list of exports a title
needs, with call counts. `--diff` between "Kinect off" and "Kinect on" isolates
exactly what the Kinect path adds, which is the plugin's required surface.

## 4. Build and run the emulator (podman, no toolchain on the host)

The host does not need a toolchain; the container provides clang 19 (see
`docs/building.md` for the required version).

```sh
./tools/nui-spike/podman-check.sh xenia-app   # full build; xenia-kernel alone is faster
./tools/nui-spike/extract-binary.sh          # -> ../xenia-nui-build/
../xenia-nui-build/run.sh --log_nui_trace 0x1F
```

Output of `extract-binary.sh`:

| path | what |
|---|---|
| `xenia-nui-build/xenia_canary` | the emulator, built with the instrumentation patch |
| `xenia-nui-build/run.sh` | wrapper, passes arguments through |
| `xenia-nui-build/lib/` | the container's shared libraries, kept for hosts that lack them |

**`run.sh` uses the host's libraries by default.** Putting the container's Debian
trixie GTK/GL ahead of the host's was observed to segfault during startup; the
mirror is opt-in via `XENIA_SPIKE_BUNDLED_LIBS=1` and is only useful on a host
that is missing something.

`run-container.sh` is the alternative: it runs the binary inside the image with
your data directory bind-mounted, which always has the right libraries.

### What the container needed beyond the docs

`docs/building.md` lists the build dependencies; these were also required:

* `make` — some `third_party` sub-projects configure with Makefiles even though
  the top level uses Ninja.
* `libsdl2-dev` — `third_party/CMakeLists.txt:462` does `pkg_check_modules(sdl2)`.
* `glslang-tools`, `spirv-tools` — `tools/build/compile_shader_spirv.py` shells out to
  `glslangValidator`, `spirv-opt` and `spirv-dis`.
* `libx11-xcb-dev` — `src/xenia/ui/window_gtk.cc`.

Two quirks the container works around, both harmless for a test binary:

* Debian's SPIRV-Tools 2025.1 dropped `spirv-opt --canonicalize-ids`, which
  `compile_shader_spirv.py` passes; the image installs a wrapper that filters the
  flag out (only ID numbering changes, not behaviour).
* `version.h` is normally generated by `xenia-build.py`; `check-build.sh` writes an
  equivalent one so cmake can be invoked directly.

### How it behaves

* the repo is mounted **read-only** at `/src` and mirrored into a podman volume
  (`xenia-nui-work`), so submodules, generated shader bytecode and the build
  directory never touch the host tree;
* submodules are initialised shallow on the first run (needs network); later runs
  reuse the volume and only recompile what changed;
* shader bytecode is generated *into the source tree* by the build, so `rsync
  --delete` must not remove it — the sync excludes `*/bytecode/` for that reason;
* the container has no Vulkan SDK: shader compilation uses Debian's glslang, which
  is close enough for a spike binary but not for a build you intend to trust.

Destroy the container afterwards:

```sh
./tools/nui-spike/teardown.sh          # image, work volume, leftover containers
./tools/nui-spike/teardown.sh --all    # also prune dangling layers
```

Check what is still there:

```sh
podman images | grep xenia-nui-spike
podman volume ls | grep xenia-nui-work
podman ps -a --filter ancestor=xenia-nui-spike:clang19
```

The image is ~1 GB (clang, GTK, ninja); the work volume grows to several GB with
submodules and build output. Both are removed by `teardown.sh`.

## 4a. The NUI surface, without running anything

```sh
python3 tools/nui-spike/gen_surface.py --csv tools/nui-spike/nui_surface.csv
```

Parses `xam_table.inc` / `xboxkrnl_table.inc` and every `DECLARE_*_EXPORT*` tag, so
it needs no build. Current result: **129 NUI-related exports — 13 implemented,
21 stub, 4 sketchy, 91 with no shim at all**. For the 91, xenia returns a null
import to the guest (`kernel_module.cc:119-134`), so a title that calls one faults.
Re-run it whenever upstream changes; it is the checklist the plugin has to work
against.

## 5. Test content: NUIView

The ideal test title is **NUIView**, the Xbox 360 Kinect *developer test
application* (title `4D5308FB`, media dated 2010-05-04, "Game Demo", ~43 MB). It
talks only to the NUI API — device status, skeleton stream, HUD, identity, camera
— so it exercises exactly the surface the spike is about, in 43 MB instead of a
multi-gigabyte retail game. Expect it to complain about a missing Kinect; the
point is which exports it calls while doing so.

If you have it, drop the XEX into the content layout xenia expects:

```
<content_root>/0000000000000000/4D5308FB/00080000/<module-hash>
```

and run the wrapper, which seeds the storage root, sets the Kinect bit and prints
the summary when you close the emulator:

```sh
./tools/nui-spike/trace-nuiview.sh on      # Kinect Initialized = ON
./tools/nui-spike/trace-nuiview.sh off     # Kinect Initialized = OFF
python3 tools/nui-spike/analyze.py \
    ../xenia-nui-build/storage-off/nui_trace/trace.tsv \
    ../xenia-nui-build/storage-on/nui_trace/trace.tsv --diff
```

Each run gets its own storage root (`../xenia-nui-build/storage-{on,off}`), so the
Kinect bit and the traces never mix. It must be run from a real terminal — the
emulator needs a display and exits silently otherwise.

`set-kinect-flag.py` writes `X_RETAIL_FLAGS::KinectInitialized` straight into
`xconfig.settings` (byte offset 2334, big-endian), which is the same thing the
Console Settings checkbox does, so the A/B pair does not depend on clicking
through the UI.

### If you do not have NUIView

No public Xbox 360 homebrew uses the NUI API, and the only copies of NUIView
around are on ROM-dump sites. The legitimate alternative is building a probe
yourself: [josevolpato/xenon-toolchain](https://github.com/josevolpato/xenon-toolchain)
(GCC 10.2 + newlib, GPL-3.0, XDK-free) plus
[libxenon](https://github.com/xenia-project/libxenon), writing a ~200-line
`nui-probe.xex` that calls `XamNuiGetDeviceStatus`, `XamNuiIsDeviceReady`,
`XamNuiPlayerEngagementUpdate`, `XMsgSystemProcessCall(0xFE, 0x2B003, …)` and
`XNotifyBroadcast`. xenia launches a bare `.xex` directly
(`emulator.cc:1550-1554`), so no game is needed. Open question: whether
libxenon's import stub covers `xam.xex` or only `xboxkrnl`.

## 7. Findings (Phase 0)

Each of these cost a build/run cycle or a source dive, so they are recorded here.

| Finding | Where |
|---|---|
| **Wayland sessions abort the emulator.** The Vulkan presenter only accepts an XCB window (`window_gtk.cc:280-293`); under Wayland GDK provides a Wayland surface, `CreateSurface` returns nullptr and xenia aborts after `Loading module ...`. `GDK_BACKEND=x11` gets past that, but the session then produced a black screen and a crash when opening settings, so **the Linux build is not a reliable way to finish this spike** — use the Windows CI workflow. | — |
| **Arguments need `--`.** `-storage_root` parses as short options: `Option 's' does not exist`. Bool cvars take no value: `--nui_fake_device`, not `--nui_fake_device true` (the `true` is taken as the path). | — |
| **`0x1F` includes `0x10`**, i.e. trace-everything. Use `0xF` for the NUI-only view. | — |
| **`log_nui_trace` gets reset mid-session.** xenia persists `log_nui_trace = 0` into `xenia-canary.config.toml` and reloads it while a title boots, silently disabling tracing after a few hundred events. The tracer now captures its flags once at `Init()`. | — |
| Trace timestamps originally used `Clock::QueryHostTickCount()`, which is the *guest* tick counter and barely advances while the guest idles; now wall clock. | — |
| **NUIView imports 35 unimplemented functions**, three of them NUI-critical and fatal (`PsCamDeviceRequest` 0x30D, `McaDeviceRequest` 0x30E, `DetroitDeviceRequest` 0x30F): in `xboxkrnl_table.inc` with no implementation, so the guest got a null import and an `undefined extern call`. Fixed by `0002-nui-device-request-stubs.patch`, which adds them plus an `--nui_fake_device` knob to report the device as present. | `xboxkrnl_table.inc:795-797` |
| With the stubs in place and the Kinect flag set, NUIView runs to timeout without crashing and calls **no** `XamNui*` export — it concludes there is no camera from the `PsCam`/`Mca` replies. `--nui_fake_device` flips those replies to "present" so the app walks its NUI init path; that measurement is the one still outstanding. | — |

### You do not need a live run to get the surface list

xenia already dumps a module's import table, marking unimplemented exports with `!!`
(`user_module.cc:1199`). **That dump is the title's required surface**, and it is
available from a log of a run that gets as far as `Loading module` — no NUI
behaviour needed. NUIView's entire NUI surface is five functions:

| Ordinal | Export | State in xenia |
|---|---|---|
| 0x873 | `XamNuiIdentityGetSessionId` | implemented |
| 0x869 | `XamUserNuiGetUserIndex` | stub (`X_E_FAIL`) |
| 0x30D | `PsCamDeviceRequest` | null import → stubbed by patch 0002 |
| 0x30E | `McaDeviceRequest` | null import → stubbed by patch 0002 |
| 0x30F | `DetroitDeviceRequest` | null import → stubbed by patch 0002 |

The other 30 unimplemented imports are `NetDll_XHttp*`, `Io*`/`Ob*`, `LDI*`,
`RtlUnwind`, `Stfs*`, `XeKeysConsoleSignatureVerification` — not Kinect-related.

## 8. Getting a Windows binary

The Linux build is too flaky to iterate on (see above). `.github/workflows/NuiSpikeWindows.yml`
builds the instrumented `xenia_canary.exe` on a GitHub runner:

```sh
./tools/nui-spike/push-and-build.sh <your-github-user>
```

That creates the `nui-spike` branch, commits only `tools/nui-spike/`,
`docs/kinect_support.md` and the workflow, adds your fork as the `fork` remote and
pushes (after a confirmation prompt). Then:

1. Actions → **NUI Spike (Windows)** → Run workflow — or just wait for the push
   trigger, since the workflow also fires on pushes to `nui-spike`.
2. Download the artifact, from the browser or with the CLI:

   ```sh
   gh run download <run-id> --repo <user>/xenia-canary --name xenia_canary_nui
   ```

   It unzips to `spike/xenia_canary.exe` plus the analysis tooling. A build takes
   ~25 min, dominated by the shallow submodule clones and the full `xenia-app`
   compile.

Two booleans on the dispatch control what gets applied: `apply_trace` (patch 0001,
the tracer) and `apply_nui_stubs` (patch 0002, the `PsCam`/`Mca`/`Detroit`
device requests). Both default to true — including on push-triggered runs, where
the dispatch inputs are unset and would otherwise silently apply nothing.

The workflow applies the patches itself and commits nothing, so the branch you
push has no emulator source changes. A verification step fails the run if
`src/xenia/kernel/nui_trace.cc` or `xboxkrnl_nui.cc` is missing, so a stale patch
can't quietly produce an uninstrumented binary.

On Windows:

```
xenia_canary.exe --log_nui_trace 0x0F -storage_root .\spike <title>
python tools\nui-spike\analyze.py .\spike\nui_trace\trace.tsv
```

### Cross-compiling Windows from Linux: not viable, use a VM instead

Checked on this machine: no `zig`, no `mingw-w64`, no `clang-cl`, no `wine`, no
`qemu` — but `/dev/kvm` **is** available.

A mingw-w64 cross build of xenia is not realistic: the Windows build needs
`d3d12.lib`/`dxgi.lib`/`xaudio2.lib` (mingw does not ship D3D12 or DXGI import
libs), `fxc.exe` from the Windows SDK for shader compilation, and it leans on
MSVC-specific machinery throughout. Getting clang-cl + an SDK working locally is
possible in principle (`xwin` fetches the SDK/CRT headers from Microsoft's CDN)
but is a multi-hour experiment with a low success rate, for a build that CI
produces in ~15 minutes.

If you want a *permanent local* Windows build path, the realistic option is a
Windows VM under `/dev/kvm` — same MSVC toolchain upstream uses, so nothing about
the build changes:

```sh
qemu-system-x86_64 -enable-kvm -m 16G -smp 8 \
  -drive file=windows.qcow2,if=virtio -cdrom Win11.iso \
  -device virtio-vga -display gtk
```

Clone the repo into the VM, apply the patch, `python xenia-build.py build`. Mount a
shared folder for the `plugins/` and `patches/` directories so spike work stays on
the host.

## 9. What a good result looks like

* `STATE retail_flags` shows `KinectInitialized=1` in the "on" run — proves the
  checkbox is what flips the guest's belief.
* `--diff` output is a short, stable list of exports that only appear in the "on"
  run. That list is the plugin contract.
* `XamNuiIsDeviceReady` returning `0` in both runs confirms the bug noted in
  `docs/kinect_support.md` §1 — if a title then refuses to proceed, it needs either
  the §8 fallback PR or a title patch.