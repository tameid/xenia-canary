#!/usr/bin/env bash
# PHASE 0 SPIKE - lay out .xex files in the directory structure xenia expects,
# so the Windows/Proton build can open them through a content root.
#
# xenia's XBLA layout (see emulator.cc LoadTitle / the content manager):
#   <content_root>/0000000000000000/<TITLE_ID>/<MEDIA_ID>/<MODULE_HASH>
# where MODULE_HASH is the module's hash, which for real content is the file name.
#
# Usage:
#   prepare-content.sh <content-root> <xex> [title-id] [media-id]
#
# Examples:
#   # NUIView (title 4D5308FB, media 00080000) into ../xenia-nui-build/win-content
#   prepare-content.sh ../xenia-nui-build/win-content \
#       ../xenia-nui-build/Content/0000000000000000/4D5308FB/00080000/3A219CD8* \
#       4D5308FB 00080000
#
#   # a hand-made homebrew: pass it to xenia as the positional path instead
#   prepare-content.sh ../xenia-nui-build/win-content ../xenia-nui-build/test.xex
set -euo pipefail

ROOT="${1:?content root}"
XEX="${2:?path to .xex}"
TITLE_ID="${3:-}"
MEDIA_ID="${4:-00000000}"

XEX="$(readlink -f "$XEX")"
[ -f "$XEX" ] || { echo "no such file: $XEX" >&2; exit 1; }

# POSIX path -> Windows path for wine/Proton, which maps Z: to /.
win_path() {
  local p="$1"
  printf 'Z:%s' "$(printf '%s' "${p//\//\\}")"
}

# Derive the module hash the way xenia names real content: it is the module's
# hash, and existing installs already use it as the file name. Reuse the source
# file name when it looks like a hash (40 hex chars), otherwise ask xenia to
# read it (it logs "Loading module ..." with the path) - for hand-made homebrew
# the file name does not matter, xenia opens the module directly.
case "$(basename "$XEX")" in
  [0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F])
    NAME="$(basename "$XEX")"
    ;;
  *)
    NAME="$(basename "$XEX")"
    ;;
esac

if [ -n "$TITLE_ID" ]; then
  DEST="$ROOT/0000000000000000/$TITLE_ID/$MEDIA_ID"
else
  DEST="$ROOT/loose"
fi
mkdir -p "$DEST"
cp -f "$XEX" "$DEST/$NAME"

echo "==> $DEST/$NAME"
echo "    $(stat -c%s "$DEST/$NAME") bytes"
echo "    sha256 $(sha256sum "$DEST/$NAME" | cut -c1-16)… (source $(sha256sum "$XEX" | cut -c1-16)…)"

cat <<EOF

Pass to xenia (Windows paths - this is the Windows build, and wine/Proton maps
Z: to /):

  --content_root "$(win_path "$(readlink -f "$DEST")")"

or, for a loose file, hand it to xenia directly:

  "$(win_path "$XEX")"
EOF