#!/usr/bin/env bash
# PHASE 0 SPIKE - host-side wrapper: builds the container image if needed and runs
# the compile-check in an ephemeral container.
#
#   ./tools/nui-spike/podman-check.sh                 # check xenia-kernel
#   ./tools/nui-spike/podman-check.sh xenia-app       # check another target
#
# Nothing is left behind on the host except the image itself; see teardown.sh.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMAGE="xenia-nui-spike:clang19"

podman build -t "$IMAGE" -f "$HERE/Containerfile" "$HERE" >/dev/null

# --work keeps the mirrored source, submodules and build dir between runs so
# repeated checks are incremental. Drop ./teardown.sh --all to reclaim the space.
exec podman run --rm \
  --security-opt label=disable \
  -v xenia-nui-work:/work \
  -v "$REPO":/src:ro \
  -v "$HERE/check-build.sh":/work/check-build.sh:ro \
  -w /src \
  "$IMAGE" \
  bash /work/check-build.sh /src "$@"