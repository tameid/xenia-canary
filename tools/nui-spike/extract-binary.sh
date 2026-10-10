#!/usr/bin/env bash
# PHASE 0 SPIKE - copy the emulator binary out of the build volume, together with
# the shared libraries it needs, into a directory on the host that can run it.
#
#   ./tools/nui-spike/extract-binary.sh [output-dir]
#
# Default output dir: ../xenia-nui-build (next to the repo, so the repo stays clean).
# Produces:
#   xenia-nui-build/xenia_canary        the binary
#   xenia-nui-build/lib/...             shared libraries the host may not have
#   xenia-nui-build/run.sh              wrapper setting LD_LIBRARY_PATH
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$REPO/../xenia-nui-build}"
IMAGE="xenia-nui-spike:clang19"

mkdir -p "$OUT/lib"
echo "==> copying binary to $OUT"

podman run --rm -i --security-opt label=disable \
  -v xenia-nui-work:/w \
  -v "$OUT":/out \
  "$IMAGE" bash -s <<'PODMAN'
set -e
BIN=$(find /w/repo/build -maxdepth 3 -name 'xenia_canary*' -type f | head -1)
if [ -z "$BIN" ]; then
  echo "no xenia_canary binary in the build volume - run podman-check.sh first" >&2
  exit 1
fi
echo "    found: $BIN"
cp "$BIN" /out/xenia_canary
chmod +x /out/xenia_canary

# Copy every shared library the binary links against, keeping its absolute path so
# the wrapper can just point LD_LIBRARY_PATH at the mirror.
ldd /out/xenia_canary | awk '/=> \//{print $3} /^\t\//{print $1}' | sort -u | while read -r lib; do
  [ -f "$lib" ] || continue
  mkdir -p "/out/lib$(dirname "$lib")"
  cp -n "$lib" "/out/lib$lib" 2>/dev/null || true
done

# Also walk the *transitive* dependencies: some (libpulse via SDL, GTK helper
# libs) are dlopen()ed at runtime and never show up in a direct ldd, so the
# binary fails to start without them.
queue="/out/xenia_canary"
while [ -n "$queue" ]; do
  next=""
  for bin in $queue; do
    ldd "$bin" 2>/dev/null | awk '/=> \//{print $3} /^\t\//{print $1}' | while read -r lib; do
      [ -f "$lib" ] || continue
      mkdir -p "/out/lib$(dirname "$lib")"
      if [ ! -e "/out/lib$lib" ]; then
        cp "$lib" "/out/lib$lib"
        echo "$lib"
      fi
    done
  done > /tmp/newlibs
  queue="$(cat /tmp/newlibs)"
done
echo "    libraries: $(find /out/lib -name '*.so*' | wc -l)"

# dlopen()ed libraries never appear in ldd, so they have to be listed by hand.
# GTK/SDL pull these in depending on the session type (note: some live in
# subdirectories, e.g. .../pulseaudio/libpulsecommon-*.so).
find /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu -maxdepth 3 \
     \( -name 'libpulse*' -o -name 'libwayland-*' -o -name 'libasound*' \) 2>/dev/null |
while read -r lib; do
  [ -f "$lib" ] || continue
  mkdir -p "/out/lib$(dirname "$lib")"
  cp -P "$lib" "/out/lib$lib" 2>/dev/null || true
done
echo "    dlopen extras: $(find /out/lib -name 'libpulse*' | wc -l) pulse, $(find /out/lib -name 'libwayland-*' | wc -l) wayland"
PODMAN

cat > "$OUT/run.sh" <<'EOF'
#!/usr/bin/env bash
# Runs the spike binary.
# Any extra arguments are passed through, e.g.:
#   ./run.sh -log_nui_trace 0x1F some.xex
#
# By default the HOST libraries are used. The binary links against the distro's
# libc++/GTK/GL, and putting the container's Debian trixie copies ahead of the
# host's was observed to segfault during startup on this machine. The bundled
# mirror is only a fallback for hosts missing something; opt in with:
#   XENIA_SPIKE_BUNDLED_LIBS=1 ./run.sh ...
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# When bundling, every directory in the mirror goes on the search path: a few
# libraries (libpulsecommon, wayland bits) live in subdirectories the dynamic
# loader does not search by default.
if [ "${XENIA_SPIKE_BUNDLED_LIBS:-0}" = "1" ] && [ -d "$HERE/lib" ]; then
  BUNDLED="$(find "$HERE/lib" -type d -printf '%p:' | sed 's/:$//')"
  export LD_LIBRARY_PATH="$BUNDLED${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec "$HERE/xenia_canary" "$@"
EOF
chmod +x "$OUT/run.sh"

echo "==> done"
echo "    $OUT/xenia_canary"
echo "    $OUT/run.sh -log_nui_trace 0x1F"