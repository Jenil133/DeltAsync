#!/usr/bin/env python3
"""Turn bench/results/netem.csv into a Markdown table.

One table row per (delay, loss, scenario). When the CSV holds several runs
of the same point, numeric columns show the median and the "runs" column
says how many runs it summarises. Runs that failed (non-zero exit code) are
left out of the medians, except for the "interrupted" scenario, where the
failure is the point; the "ok" column shows how many runs succeeded and
left the receiver's file identical to the sender's.

Usage:
  csv_to_markdown.py [CSV] [--output FILE]

CSV defaults to bench/results/netem.csv; the table goes to standard output
unless --output is given.
"""

import argparse
import csv
import os
import statistics
import sys

SCENARIO_ORDER = {"whole-file": 0, "delta": 1, "interrupted": 2, "resumed": 3}

# (header, CSV column, formatter)
COLUMNS = [
    ("socket bytes", "socket_bytes", "bytes"),
    ("sent", "bytes_sent", "bytes"),
    ("received", "bytes_received", "bytes"),
    ("resumed from", "resumed_from", "bytes"),
    ("push elapsed ms", "elapsed_ms", "int"),
    ("wall ms", "wall_ms", "int"),
]


def number(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def format_value(value, kind):
    if value is None:
        return "-"
    if kind == "bytes" or kind == "int":
        return "{:,}".format(int(round(value)))
    return "%g" % value


def sort_key(key):
    delay, loss, scenario = key
    return (number(delay) or 0.0, number(loss) or 0.0, SCENARIO_ORDER.get(scenario, 99), scenario)


def summarise(rows):
    groups = {}
    for row in rows:
        key = (row["delay_ms"], row["loss_pct"], row["scenario"])
        groups.setdefault(key, []).append(row)

    lines = []
    headers = ["delay ms", "loss %", "scenario", "runs", "ok"] + [c[0] for c in COLUMNS] + ["notes"]
    lines.append("| " + " | ".join(headers) + " |")
    lines.append("| " + " | ".join(["---:", "---:", "---"] + ["---:"] * (len(headers) - 4)
                                   + ["---"]) + " |")
    for key in sorted(groups, key=sort_key):
        group = groups[key]
        delay, loss, scenario = key
        ok = sum(1 for r in group if r.get("exit_code") == "0" and r.get("verified") == "yes")
        if scenario == "interrupted":
            measured = group
        else:
            measured = [r for r in group if r.get("exit_code") == "0"]
        cells = [delay, loss, scenario, str(len(group)), str(ok)]
        for _, column, kind in COLUMNS:
            values = [number(r.get(column)) for r in measured]
            values = [v for v in values if v is not None]
            cells.append(format_value(statistics.median(values) if values else None, kind))
        notes = sorted({r.get("note", "") for r in group if r.get("note")})
        if scenario == "interrupted":
            # Every interrupted run has its own byte count in the note; keep
            # the table readable by not repeating them.
            notes = [n for n in notes if not n.startswith("proxy forwarded")]
        cells.append("; ".join(notes).replace("|", "/"))
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def main(argv):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    default_csv = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench",
                               "results", "netem.csv")
    parser.add_argument("csv", nargs="?", default=default_csv)
    parser.add_argument("--output", help="write the table here instead of standard output")
    args = parser.parse_args(argv)

    with open(args.csv, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print("csv_to_markdown: %s has no data rows" % args.csv, file=sys.stderr)
        return 1
    table = summarise(rows)
    if args.output:
        with open(args.output, "w") as f:
            f.write(table)
    else:
        sys.stdout.write(table)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
