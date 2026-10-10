#!/usr/bin/env bash
# PHASE 0 SPIKE - run the Windows xenia build on Linux through Steam's Proton.
#
# Steam's Proton sandbox confines a non-Steam shortcut to Steam's own directories,
# so xenia cannot reach your content when launched that way ("it can only access
# what it created itself"). Invoking the same Proton build directly skips the
# sandbox and gives the process a full Z: -> / mapping.
#
# The storage root is self-contained:
#   winstore/content/0000000000000000/4D5308FB/00080000/<hash>   NUIView
#   winstore/test.xex                                             hand-made homebrew
#
# Usage:
#   run-nuiview-proton.sh [--fake-device] [--trace-mask N] [--title test|nuiview] [--seconds N]
#
# Examples:
#   ./tools/nui-spike/run-nuiview-proton.sh
#   ./tools/nui-spike/run-nuiview-proton.sh --fake-device
#   ./tools/nui-spike/run-nuiview-proton.sh --title test --seconds 60
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="${XENIA_SPIKE_BUILD_DIR:-$REPO/../xenia-nui-build}"
EXE="$BUILD/windows/build/bin/Windows/Release/xenia_canary.exe"
STORE="$BUILD/winstore"
STEAM_ROOT="$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
PREFIX="${XENIA_SPIKE_PROTON_DATA:-/tmp/opencode/pdata}"

FAKE=0
MASK=0x0F
TITLE=nuiview
SECONDS_TO_RUN=110

while [ $# -gt 0 ]; do
  case "$1" in
    --fake-device) FAKE=1; shift;;
    --trace-mask) MASK="$2"; shift 2;;
    --title) TITLE="$2"; shift 2;;
    --seconds) SECONDS_TO_RUN="$2"; shift 2;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done

[ -f "$EXE" ] || { echo "no Windows build at $EXE (gh run download ...)" >&2; exit 1; }
# The artifact comes out of a .7z without the exec bit; proton does not care but
# our own test does.
chmod +x "$EXE" 2>/dev/null || true
[ -d "$STORE" ] || { echo "no storage root at $STORE" >&2; exit 1; }

PROTON="$STEAM_ROOT/steamapps/common/Proton 9.0 (Beta)/proton"
[ -x "$PROTON" ] || PROTON="$STEAM_ROOT/steamapps/common/Proton - Experimental/proton"
[ -x "$PROTON" ] || { echo "no Proton build found under $STEAM_ROOT" >&2; exit 1; }

# Proton wants the compat data directory to exist; it creates the prefix inside.
mkdir -p "$PREFIX"

case "$TITLE" in
  nuiview)
    TITLE_PATH="$STORE/content/0000000000000000/4D5308FB/00080000/3A219CD8ADA8B57F244A06C8EF49B9A4E3191EF94D"
    ;;
  test)
    TITLE_PATH="$STORE/test.xex"
    ;;
  *)
    TITLE_PATH="$TITLE"
    ;;
esac

# POSIX path -> Windows path (wine maps Z: to /).
win() { printf 'Z:%s' "$(printf '%s' "${1//\//\\}")"; }

ARGS=(
  --enable_console
  --log_level 3
  "--log_nui_trace" "$MASK"
  "--log_file" "$(win "$STORE/xenia.log")"
  "--storage_root" "$(win "$STORE")"
)
[ "$FAKE" = 1 ] && ARGS+=(--nui_fake_device)
ARGS+=("$(win "$TITLE_PATH")")

echo "==> $PROTON"
echo "==> title:  $TITLE_PATH"
echo "==> fake device: $FAKE   trace mask: $MASK   seconds: $SECONDS_TO_RUN"

cd "$(dirname "$EXE")"
timeout "$SECONDS_TO_RUN" env \
  STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT" \
  STEAM_COMPAT_DATA_PATH="$PREFIX" \
  "$PROTON" run "$EXE" "${ARGS[@]}" || echo "(exit $?)"

echo
echo "==> trace:"
python3 "$HERE/analyze.py" "$STORE/nui_trace/trace.tsv" 2>/dev/null || echo "no trace"
echo "==> log: $STORE/xenia.log"