#!/usr/bin/env bash
# PHASE 0 SPIKE - push the spike branch to YOUR fork and print the Actions URL.
#
# You need a fork of xenia-canary/xenia-canary (this repo's only remote is
# upstream, which you cannot push to) and a PAT/GitHub auth for it.
#
# Usage:
#   ./tools/nui-spike/push-and-build.sh <github-user> [remote-name]
#
# Example:
#   ./tools/nui-spike/push-and-build.sh myhandle
#
# Nothing is pushed unless you confirm at the prompt.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
USER="${1:?usage: push-and-build.sh <github-user> [remote-name]}"
REMOTE="${2:-fork}"
BRANCH="nui-spike"

cd "$REPO"

if [ "$(git rev-parse --abbrev-ref HEAD)" != "$BRANCH" ]; then
  echo "==> creating/switching to branch $BRANCH (current: $(git rev-parse --abbrev-ref HEAD))"
  git switch -c "$BRANCH" 2>/dev/null || git switch "$BRANCH"
fi

echo "==> files that will be pushed:"
git status --short | grep -E '^\?\?' || true
git ls-files --others --exclude-standard | grep -E 'tools/nui-spike|docs/kinect_support.md|.github/workflows/NuiSpikeWindows.yml' || true

git add .github/workflows/NuiSpikeWindows.yml tools/nui-spike docs/kinect_support.md
echo "==> staged:"
git diff --cached --stat

if [ -z "$(git diff --cached --name-only)" ]; then
  echo "nothing to commit - already committed?" >&2
  exit 1
fi

read -r -p "==> commit and push to https://github.com/$USER/xenia-canary.git? [y/N] " answer
case "$answer" in
  [yY]|[yY][eE][sS]) ;;
  *) echo "aborted"; exit 1 ;;
esac

git commit -q -m "[Spike] NUI trace instrumentation tooling and Windows build workflow

Adds the Phase 0 spike for Kinect/NUI support:
 - tools/nui-spike/0001-nui-trace-instrumentation.patch (throwaway tracing)
 - tools/nui-spike/0002-nui-device-request-stubs.patch (PsCam/Mca/Detroit)
 - trace analysis + surface-table tooling
 - docs/kinect_support.md design document
 - .github/workflows/NuiSpikeWindows.yml builds an instrumented xenia_canary.exe

The emulator sources are not modified in-tree; the workflow applies the patches
before building."

if git remote get-url "$REMOTE" >/dev/null 2>&1; then
  echo "==> updating existing remote $REMOTE"
  git remote set-url "$REMOTE" "git@github.com:$USER/xenia-canary.git"
else
  git remote add "$REMOTE" "git@github.com:$USER/xenia-canary.git"
  echo "==> added remote $REMOTE (ssh)"
fi

git push -u "$REMOTE" "$BRANCH"

cat <<EOF

==> pushed. Two ways to build:

1. It may already be running (the workflow triggers on pushes to $BRANCH):
   https://github.com/$USER/xenia-canary/actions

2. Or run it manually:
   https://github.com/$USER/xenia-canary/actions/workflows/NuiSpikeWindows.yml
   (Actions -> NUI Spike (Windows) -> Run workflow)

When it finishes, download the 'xenia_canary_nui' artifact from the run summary.
EOF