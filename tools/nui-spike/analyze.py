#!/usr/bin/env python3
"""PHASE 0 SPIKE - analyze an NUI trace produced by the instrumentation patch.

The trace is TSV produced by src/xenia/kernel/nui_trace.cc:

    seq <TAB> us <TAB> thread <TAB> kind <TAB> name <TAB> ordinal <TAB> detail

Usage:
  analyze.py trace.tsv                     summary of one run
  analyze.py trace.tsv --csv surface.csv   write the call surface as CSV
  analyze.py a.tsv b.tsv --diff            what a run does that another does not
  analyze.py trace.tsv --hexdump 0x2B003   decoded buffer of one message id
"""

import argparse
import collections
import csv
import re
import sys

KINECT_MESSAGE_RANGES = ((0x2B000, 0x2BFFF), (0x2C000, 0x2CFFF))


def parse(path):
    rows = []
    try:
        handle = open(path, encoding="utf-8", errors="replace")
    except FileNotFoundError:
        print(f"no trace at {path}", file=sys.stderr)
        print(
            "hint: the tracer only writes when -log_nui_trace is non-zero and the\n"
            "      xam module loads; make sure the emulator actually started (it needs\n"
            "      a display - run it from a real terminal, not a detached shell).",
            file=sys.stderr,
        )
        return []
    with handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 7:
                continue
            seq, us, thread, kind, name, ordinal, detail = parts[:7]
            rows.append(
                {
                    "seq": int(seq),
                    "us": int(us),
                    "thread": int(thread),
                    "kind": kind,
                    "name": name,
                    "ordinal": ordinal,
                    # Detail may itself contain tabs (never in practice, but be safe).
                    "detail": "\t".join(parts[6:]),
                }
            )
    return rows


def is_kinect_message(ordinal_str):
    try:
        value = int(ordinal_str, 16)
    except ValueError:
        return False
    return any(lo <= value <= hi for lo, hi in KINECT_MESSAGE_RANGES)


def summarize(rows, title):
    print(f"=== {title} ===")
    print(f"lines: {len(rows)}")

    span = (rows[-1]["us"] - rows[0]["us"]) / 1e6 if rows else 0.0
    print(f"span:  {span:.1f}s")

    kinds = collections.Counter(r["kind"] for r in rows)
    print("kinds: " + ", ".join(f"{k}={v}" for k, v in sorted(kinds.items())))

    for kind in ("STATE",):
        for row in rows:
            if row["kind"] == kind:
                print(f"state: {row['name']} {row['detail']}")

    print("\nexports (name, calls, distinct results):")
    calls = collections.Counter()
    results = collections.defaultdict(set)
    for row in rows:
        if row["kind"] == "EXPORT_IN":
            calls[row["name"]] += 1
        elif row["kind"] == "EXPORT_OUT":
            match = re.search(r"ret=([0-9A-Fa-f]+)", row["detail"])
            results[row["name"]].add(match.group(1) if match else "?")
    for name, count in calls.most_common():
        rets = ",".join(sorted(results.get(name, {"-"})))
        print(f"  {count:6d}  {name:52s} ret={rets}")

    messages = collections.Counter(
        (r["name"], r["ordinal"]) for r in rows if r["kind"] == "MESSAGE"
    )
    nui_messages = [(k, v) for k, v in messages.items() if is_kinect_message(k[1])]
    print(f"\nmessages: {sum(messages.values())} total, "
          f"{sum(v for _, v in nui_messages)} in the 0x2B/0x2C Kinect ranges")
    for (name, ordinal), count in sorted(nui_messages, key=lambda kv: -kv[1]):
        print(f"  {count:6d}  {name} {ordinal}")

    notifies = collections.Counter(
        (r["name"], r["ordinal"], r["detail"])
        for r in rows
        if r["kind"] == "NOTIFY"
    )
    if notifies:
        print("\nnotifications:")
        for (name, ordinal, detail), count in notifies.most_common(20):
            print(f"  {count:6d}  {name} {ordinal} {detail}")


def write_surface(rows, path):
    rows_by_call = {}
    for row in rows:
        if row["kind"] == "EXPORT_IN":
            rows_by_call[(row["name"], row["thread"], row["us"])] = row
    surface = collections.Counter(
        (r["name"], r["ordinal"]) for r in rows if r["kind"] == "EXPORT_IN"
    )
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["ordinal", "export", "calls"])
        for (name, ordinal), count in sorted(surface.items(), key=lambda kv: -kv[1]):
            writer.writerow([ordinal, name, count])
    print(f"wrote {path} ({len(surface)} distinct exports)")


def diff_runs(a, b):
    def surface(rows):
        return collections.Counter(
            (r["name"], r["ordinal"]) for r in rows if r["kind"] == "EXPORT_IN"
        )

    sa, sb = surface(a), surface(b)
    only_a = {k: v for k, v in sa.items() if k not in sb}
    only_b = {k: v for k, v in sb.items() if k not in sa}
    changed = {k: (sa[k], sb[k]) for k in sa if k in sb and sa[k] != sb[k]}

    print(f"only in {a[0]}: {len(only_a)} exports")
    for (name, ordinal), count in sorted(only_a.items(), key=lambda kv: -kv[1])[:60]:
        print(f"  + {count:6d} {ordinal} {name}")
    print(f"\nonly in {b[0]}: {len(only_b)} exports")
    for (name, ordinal), count in sorted(only_b.items(), key=lambda kv: -kv[1])[:60]:
        print(f"  + {count:6d} {ordinal} {name}")
    print(f"\ncall count changed: {len(changed)} exports")
    for (name, ordinal), (ca, cb) in sorted(changed.items(), key=lambda kv: -kv[1][0])[:40]:
        print(f"  {ca:6d} -> {cb:6d} {ordinal} {name}")


def hexdump(rows, wanted):
    want = int(wanted, 16)
    for row in rows:
        if row["kind"] != "MESSAGE":
            continue
        if int(row["ordinal"], 16) != want:
            continue
        match = re.search(r"data=(.*)$", row["detail"])
        print(f"seq={row['seq']} t={row['us'] / 1e6:.3f}s {row['detail']}")
        if match:
            # Tokens look like "0000:00000000" (offset marker) or bare bytes.
            tokens = match.group(1).split()
            for offset in range(0, len(tokens), 8):
                print("    " + " ".join(tokens[offset:offset + 8]))
        print()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+", help="trace .tsv file(s)")
    parser.add_argument("--csv", help="write the call surface to this CSV")
    parser.add_argument("--diff", action="store_true", help="diff two runs")
    parser.add_argument("--hexdump", help="decode buffers of this message id")
    args = parser.parse_args()

    if args.hexdump:
        hexdump(parse(args.traces[0]), args.hexdump)
        return
    if args.diff and len(args.traces) == 2:
        diff_runs(parse(args.traces[0]), parse(args.traces[1]))
        return

    rows = parse(args.traces[0])
    if not rows:
        return 1
    rows.sort(key=lambda r: r["seq"])
    rows[0]["_path"] = args.traces[0]
    summarize(rows, args.traces[0])
    if args.csv:
        write_surface(rows, args.csv)


if __name__ == "__main__":
    main()