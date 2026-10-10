#!/usr/bin/env python3
"""Verify the Phase 0 spike patches before/after they are applied.

Every failure the spike workflow hit during Phase 0 passed `git apply --check`
and only surfaced minutes into a 20-minute build:

  1. a verification regex matched the patch's own explanatory comment,
  2. a hand-edited patch left its `@@` line count stale, so `git apply` wrote
     only the first 321 of 330 lines and truncated nui_trace.cc mid-function,
  3. `DECLARE_bool` sat inside namespace xam while 0002 defined the cvar at
     global scope, so it compiled fine and failed at link with LNK2001.

None of these need a compiler. This checks them in under a second:

  python3 tools/nui-spike/verify_patches.py tools/nui-spike/0001-*.patch ...
      Validate patch files: every @@ hunk header must agree with the number of
      lines the hunk actually contains. Catches (2).

  python3 tools/nui-spike/verify_patches.py --applied
      Validate the working tree: files exist, braces balance, the cvar is
      declared at global scope, the fixed function no longer has the bug.
      Catches (1) and (3).

Exits non-zero on the first failure, printing what to fix.
"""

import os
import re
import sys

# Patches that must be present once the spike tree is built, and the invariant
# each one is responsible for establishing.
REQUIRED_AFTER_APPLY = {
    "src/xenia/kernel/nui_trace.cc": {"min_lines": 300},
    "src/xenia/kernel/xboxkrnl/xboxkrnl_nui.cc": {"min_lines": 50},
}

HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def check_patch_hunks(path):
    """Every @@ header must agree with the lines the hunk really contains.

    git apply trusts the header. A stale count is silent data loss: the patch
    applies "successfully" and writes a truncated file.
    """
    errors = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.read().split("\n")

    header = None
    old_declared = new_declared = 0
    old_left = new_left = 0
    seen = 0

    def close_hunk():
        """Check the open hunk, if any, and clear the tracking state.

        old_left/new_left are decremented per line and should both reach zero.
        Anything left over means the header and the body disagree.
        """
        if header is None:
            return
        if old_left or new_left:
            errors.append(
                "%s: hunk header says -%d/+%d but the body holds %d old and %d "
                "new lines; git apply would write a truncated file (%s)"
                % (
                    path,
                    old_declared,
                    new_declared,
                    old_declared - old_left,
                    new_declared - new_left,
                    header,
                )
            )

    for line in lines:
        match = HUNK_RE.match(line)
        if match:
            close_hunk()
            header = line
            old_declared = int(match.group(2) or 1)
            new_declared = int(match.group(4) or 1)
            old_left = old_declared
            new_left = new_declared
            seen = 0
            continue
        if header is None:
            continue
        if line.startswith("diff --git ") or line.startswith("index "):
            close_hunk()
            header = None
            continue
        if line.startswith("\\"):  # "\ No newline at end of file"
            continue
        if line == "":
            # A trailing empty string from split() is not a patch line.
            continue
        if line[0] in " -":
            old_left -= 1
        if line[0] in " +":
            new_left -= 1
        if line[0] in " +-":
            seen += 1
    close_hunk()
    return errors


def check_applied(root):
    errors = []

    for rel, rules in REQUIRED_AFTER_APPLY.items():
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            errors.append("%s is missing - the patch that creates it did not apply" % rel)
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        lines = text.split("\n")
        minimum = rules.get("min_lines")
        if minimum and len(lines) < minimum:
            errors.append(
                "%s has only %d lines (expected at least %d) - the patch looks "
                "truncated, check its @@ line counts"
                % (rel, len(lines), minimum)
            )
        opens = text.count("{")
        closes = text.count("}")
        if opens != closes:
            errors.append(
                "%s has unbalanced braces (%d open, %d close) - the patch is "
                "truncated mid-function" % (rel, opens, closes)
            )

    xam_nui = os.path.join(root, "src/xenia/kernel/xam/xam_nui.cc")
    if os.path.exists(xam_nui):
        with open(xam_nui, "r", encoding="utf-8", errors="replace") as handle:
            lines = handle.read().split("\n")
        buggy = re.compile(r"^\s*return\s+kinect_initialized\s*>>\s*1\s*&\s*1;")
        for number, line in enumerate(lines, 1):
            if buggy.match(line):
                errors.append(
                    "%s:%d still returns 'kinect_initialized >> 1 & 1' - patch "
                    "0003 did not apply" % (rel_to(xam_nui, root), number)
                )
                break
        # 0002 defines nui_fake_device at global scope, so 0003 must declare it
        # there too. Inside namespace xam it resolves to
        # xe::kernel::xam::cvars::... and fails to link with LNK2001.
        decl = None
        namespace = None
        for number, line in enumerate(lines, 1):
            if decl is None and "DECLARE_bool(nui_fake_device)" in line:
                decl = number
            if namespace is None and re.match(r"^namespace xe \{", line):
                namespace = number
        if decl is not None and namespace is not None and decl > namespace:
            errors.append(
                "xam_nui.cc declares nui_fake_device at line %d, inside "
                "namespace xe (line %d); 0002 defines it at global scope, so "
                "this will not link. Move the DECLARE_bool above the namespaces."
                % (decl, namespace)
            )
    return errors


def rel_to(path, root):
    try:
        return os.path.relpath(path, root)
    except ValueError:
        return path


def main(argv):
    if not argv:
        print(__doc__)
        return 2

    if argv[0] == "--applied":
        root = repo_root()
        errors = check_applied(root)
        if errors:
            print("PATCH VERIFICATION FAILED (applied tree):", file=sys.stderr)
            for error in errors:
                print("  - %s" % error, file=sys.stderr)
            return 1
        print("Patch verification OK (applied tree).")
        return 0

    errors = []
    for patch in argv:
        if not os.path.exists(patch):
            errors.append("%s does not exist" % patch)
            continue
        errors.extend(check_patch_hunks(patch))
    if errors:
        print("PATCH VERIFICATION FAILED:", file=sys.stderr)
        for error in errors:
            print("  - %s" % error, file=sys.stderr)
        return 1
    print("Patch verification OK (%d patch file(s))." % len(argv))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))