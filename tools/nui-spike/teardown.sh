#!/usr/bin/env bash
# Destroy everything the NUI spike created in podman.
# Safe to run at any time; no host files are touched.
set -euo pipefail

IMAGE="xenia-nui-spike:clang19"

echo "==> removing containers from the spike image"
ids="$(podman ps -aq --filter "ancestor=$IMAGE" 2>/dev/null || true)"
if [ -n "$ids" ]; then
  # shellcheck disable=SC2086
  podman rm -f $ids
else
  echo "    none"
fi

echo "==> removing the image"
podman rmi -f "$IMAGE" 2>/dev/null || echo "    already gone"

echo "==> removing the work volume (mirrored source + build dir, can be several GB)"
podman volume rm -f xenia-nui-work 2>/dev/null || echo "    already gone"

if [ "${1:-}" = "--all" ]; then
  echo "==> pruning dangling podman layers"
  podman image prune -f
fi

echo "==> done"