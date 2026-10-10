#!/usr/bin/env bash
# PHASE 0 SPIKE - run NUIView (the Xbox 360 Kinect developer test application,
# title 4D5308FB) under the instrumented emulator and summarise the trace.
#
# NUIView is a 43 MB XBLA demo that talks *only* to the NUI API, which makes it
# the ideal stand-in for a retail NUI game: no game content needed, and it will
# call the surface we care about (device status, skeletons, HUD, identity).
#
# Must be run from a real terminal: the emulator needs a display and this shell
# script does not start one.
#
# Usage:
#   ./tools/nui-spike/trace-nuiview.sh on      # Kinect Initialized = ON
#   ./tools/nui-spike/trace-nuiview.sh off     # Kinect Initialized = OFF
#   ./tools/nui-spike/trace-nuiview.sh auto    # use an existing XEX path
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="${XENIA_SPIKE_BUILD_DIR:-$REPO/../xenia-nui-build}"
XEX="$BUILD/Content/0000000000000000/4D5308FB/00080000/3A219CD8ADA8B57F244A06C8EF49B9A4E3191EF94D"
STATE="${1:-on}"
STORAGE="$BUILD/storage-$STATE"

[ -x "$BUILD/xenia_canary" ] || { echo "no binary in $BUILD - run extract-binary.sh" >&2; exit 1; }
[ -f "$XEX" ] || XEX="$(find "$BUILD/Content" -name '3A219*' -o -name '*.xex' 2>/dev/null | head -1)"
[ -f "$XEX" ] || { echo "cannot find the NUIView xex under $BUILD/Content" >&2; exit 1; }

echo "==> emulator: $BUILD/xenia_canary"
echo "==> title:    $XEX"
echo "==> storage:  $STORAGE  (Kinect Initialized = $STATE)"

# Base the storage root on an existing xconfig so language/region/resolution stay
# sane, then set the Kinect bit (or leave it clear).
mkdir -p "$STORAGE"
if [ ! -f "$STORAGE/xconfig.settings" ]; then
  BASE="$(ls "$HOME"/.local/share/Xenia/xconfig.settings 2>/dev/null || true)"
  if [ -n "$BASE" ]; then
    cp "$BASE" "$STORAGE/xconfig.settings"
  else
    : > "$STORAGE/xconfig.settings"   # the emulator fills in defaults on first run
  fi
fi
python3 "$HERE/set-kinect-flag.py" "$STORAGE/xconfig.settings" \
  "$([ "$STATE" = off ] && echo off || echo on)"

rm -f "$STORAGE/nui_trace/trace.tsv"

cat <<EOF

----------------------------------------------------------------------
Starting the emulator. Interact with NUIView for ~30 seconds, then close
it. The trace is written to:
    $STORAGE/nui_trace/trace.tsv
----------------------------------------------------------------------

EOF

# Xenia's Vulkan presenter only supports an XCB window (window_gtk.cc:280-293).
# On a Wayland session GDK hands it a Wayland surface, CreateSurface returns
# nullptr and the emulator aborts right after "Loading module ...". Force X11;
# XWayland is fine for this, it is GDK's backend choice that matters.
export GDK_BACKEND="${GDK_BACKEND:-x11}"

"$BUILD/run.sh" \
  --storage_root "$STORAGE" \
  --content_root "$BUILD/Content" \
  --log_nui_trace 0x1F \
  --log_level 3 \
  --log_file "$STORAGE/xenia.log" \
  "$XEX"

echo
echo "==> trace summary"
if [ -f "$STORAGE/nui_trace/trace.tsv" ]; then
  python3 "$HERE/analyze.py" "$STORAGE/nui_trace/trace.tsv"
else
  echo "no trace produced - did the emulator start? (needs a real terminal/display)"
fi

echo
echo "==> compare against the other state with:"
if [ "$STATE" = on ]; then
  echo "    $HERE/analyze.py $BUILD/storage-off/nui_trace/trace.tsv $STORAGE/nui_trace/trace.tsv --diff"
else
  echo "    $HERE/analyze.py $STORAGE/nui_trace/trace.tsv $BUILD/storage-on/nui_trace/trace.tsv --diff"
fi