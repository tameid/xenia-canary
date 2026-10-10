#!/usr/bin/env bash
# PHASE 0 SPIKE - run the container-built emulator inside the container.
#
# The alternative to extract-binary.sh + run.sh. Guaranteed to have the right
# libraries (the extracted copy can miss dlopen()ed ones like libpulse), at the
# cost of a little overhead. Your data directories are bind-mounted so the
# storage root, patches, plugins and trace output all live on the host:
#
#   $XDG_DATA_HOME/xenia   (or ~/.local/share/xenia)  -> /data
#   ./xenia-content        -> /content   (optional; put your xex/isos here)
#
# Usage:
#   ./tools/nui-spike/run-container.sh -log_nui_trace 0x1F
#   ./tools/nui-spike/run-container.sh -log_nui_trace 0x1F /content/some.xex
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMAGE="xenia-nui-spike:clang19"

DATA="${XDG_DATA_HOME:-$HOME/.local/share}/xenia"
CONTENT="$REPO/../xenia-nui-build/content"
mkdir -p "$DATA" "$CONTENT"

# Xenia needs a display; pass the host's through when running natively.
exec podman run --rm -it --security-opt label=disable \
  -e DISPLAY="${DISPLAY:-}" \
  -e WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" \
  -e XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}" \
  --device /dev/dri \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  -v "$DATA":/data:Z \
  -v "$CONTENT":/content:Z \
  -w /content \
  -u "$(id -u):$(id -g)" \
  "$IMAGE" \
  bash -lc '
    mkdir -p /data/.local/share/xenia
    export XDG_DATA_HOME=/data/.local/share
    exec /w/repo/build/bin/Linux/xenia_canary "$@"
  ' -- "$@"