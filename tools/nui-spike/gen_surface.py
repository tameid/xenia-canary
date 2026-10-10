#!/usr/bin/env python3
"""Generate the NUI/Kinect export surface table from the xenia sources.

No build required: it parses the ordinal table (xam_table.inc) and every
DECLARE_XAM_EXPORT* / DECLARE_XBOXKRNL_EXPORT* tag in the tree, then emits one
row per NUI-related export with its implementation status.

This is the "what exists today" reference for the Kinect spike (docs/kinect_support.md
§1): which of the ~60 NUI ordinals are implemented, which are stubs, and which are
marked high-frequency (so the trace can skip them).

Usage:
  gen_surface.py                 # print a table
  gen_surface.py --csv out.csv   # also write CSV
"""

import argparse
import csv
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

NUI_MATCH = re.compile(
    r"nui|natal|kinect|controlpack|controlpack|usbcam|biometric|hud", re.IGNORECASE
)

ORDINAL_RE = re.compile(r"XE_EXPORT\(\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)")
DECLARE_RE = re.compile(
    r"DECLARE_(XAM|XBOXKRNL|XBDM)_EXPORT(\d)\(\s*(\w+)\s*,\s*(\w+)\s*,\s*([^)]*)\)"
)

TAG_MEANING = {
    "kNone": "category",
    "kImplemented": "implemented",
    "kStub": "stub",
    "kSketchy": "sketchy",
    "kHighFrequency": "high-frequency",
    "kImportant": "important",
    "kBlocking": "blocking",
    "kIsVariable": "variable",
    "kLog": "log",
    "kLogResult": "log-result",
    "kRequiredModule": "required-module",
}


def scan_ordinals(root):
    ordinals = {}
    path = os.path.join(root, "src/xenia/kernel/xam/xam_table.inc")
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            match = ORDINAL_RE.search(line)
            if match:
                module, ordinal, name = match.groups()
                ordinals[(module.lower(), name)] = int(ordinal, 16)
    # xboxkrnl has its own table (XUsbcam lives there).
    path = os.path.join(root, "src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                match = ORDINAL_RE.search(line)
                if match:
                    module, ordinal, name = match.groups()
                    ordinals[(module.lower(), name)] = int(ordinal, 16)
    return ordinals


def scan_declarations(root):
    declarations = {}
    for sub in ("xam", "xboxkrnl", "xbdm"):
        base = os.path.join(root, "src/xenia/kernel", sub)
        for dirpath, _, filenames in os.walk(base):
            for filename in filenames:
                if not filename.endswith(".cc"):
                    continue
                full = os.path.join(dirpath, filename)
                rel = os.path.relpath(full, root)
                with open(full, encoding="utf-8", errors="replace") as handle:
                    for lineno, line in enumerate(handle, 1):
                        match = DECLARE_RE.search(line)
                        if not match:
                            continue
                        module, _, name, category, tags = match.groups()
                        tag_names = [
                            t.strip() for t in tags.split(",") if t.strip()
                        ]
                        declarations[(module.lower(), name)] = {
                            "file": rel,
                            "line": lineno,
                            "category": category,
                            "tags": tag_names,
                        }
    return declarations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=REPO)
    parser.add_argument("--csv")
    args = parser.parse_args()

    ordinals = scan_ordinals(args.repo)
    declarations = scan_declarations(args.repo)

    rows = []
    for (module, name), ordinal in sorted(ordinals.items(), key=lambda kv: kv[1]):
        if not NUI_MATCH.search(name):
            continue
        decl = declarations.get((module, name), {})
        tags = decl.get("tags", [])
        if "kStub" in tags:
            status = "stub"
        elif "kSketchy" in tags:
            status = "sketchy"
        elif "kImplemented" in tags:
            status = "implemented"
        else:
            status = "not-declared"
        rows.append(
            {
                "module": module,
                "ordinal": "0x%04X" % ordinal,
                "name": name,
                "status": status,
                "high_frequency": "yes" if "kHighFrequency" in tags else "",
                "category": decl.get("category", ""),
                "declared_in": f"{decl.get('file', '')}:{decl.get('line', '')}",
            }
        )

    if not rows:
        print("no NUI exports found", file=sys.stderr)
        return 1

    width = max(len(r["name"]) for r in rows)
    counts = {}
    for row in rows:
        counts[row["status"]] = counts.get(row["status"], 0) + 1
    print(
        f"{len(rows)} NUI-related exports: "
        + ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))
    )
    print()
    print(f"{'ordinal':<8} {'status':<12} {'name':<{width}} declared in")
    for row in rows:
        print(
            f"{row['ordinal']:<8} {row['status']:<12} {row['name']:<{width}} "
            f"{row['declared_in']}"
        )

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print(f"\nwrote {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())