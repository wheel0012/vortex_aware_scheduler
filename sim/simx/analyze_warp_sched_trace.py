#!/usr/bin/env python3

import argparse
import csv
import math
import os
import re
import sys
import tempfile
from collections import defaultdict
from pathlib import Path


MISMATCH_FIELDS = [
    "cycle",
    "policy",
    "intended_wid",
    "actual_wid",
    "pc",
    "mismatch_reason",
    "candidate_mask",
    "ready_mask",
    "ibuffer_empty",
    "fallback",
]

EVENT_INST_TYPES = {"WSPAWN", "TMC", "BAR", "FENCE", "CSRRS", "CSRRW"}


def parse_int(value):
    if value is None:
        return None
    value = str(value).strip()
    if value == "":
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


def parse_float(value):
    if value is None:
        return None
    value = str(value).strip()
    if value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def parse_bool(value):
    return str(value).strip().lower() in {"1", "true", "yes", "y"}


def parse_arg_int(value):
    return int(value, 0)


def row_cycle(row):
    return parse_int(row.get("cycle"))


def row_pc(row):
    return parse_int(row.get("pc"))


def row_actual_wid(row):
    for key in ("actual_wid", "selected_wid", "wid"):
        wid = parse_int(row.get(key))
        if wid is not None:
            return wid
    return None


def row_issued(row):
    if row.get("issued", "") != "":
        return parse_bool(row.get("issued"))
    wid = row_actual_wid(row)
    return wid is not None and wid >= 0


def row_mismatch(row):
    if row.get("mismatch", "") != "":
        return parse_bool(row.get("mismatch"))
    intended = parse_int(row.get("intended_wid"))
    actual = row_actual_wid(row)
    return intended is not None and actual is not None and actual >= 0 and intended != actual


def row_fallback(row):
    return parse_bool(row.get("fallback", ""))


def load_rows(path):
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        fieldnames = reader.fieldnames or []
    return rows, fieldnames


def select_event_cycle(rows, spec):
    if not spec:
        return None
    parts = spec.split(":", 1)
    inst_type = parts[0]
    occurrence = parts[1] if len(parts) > 1 else "first"

    cycles = []
    for row in rows:
        if not row_issued(row):
            continue
        if row.get("inst_type") != inst_type:
            continue
        cycle = row_cycle(row)
        if cycle is not None:
            cycles.append(cycle)

    if not cycles:
        raise SystemExit(f"No issued event found for {spec}")

    if occurrence == "first":
        return cycles[0]
    if occurrence == "last":
        return cycles[-1]
    try:
        index = int(occurrence)
    except ValueError as e:
        raise SystemExit(f"Invalid event occurrence in {spec}; use first, last, or 1-based index") from e
    if index <= 0 or index > len(cycles):
        raise SystemExit(f"Event occurrence {index} out of range for {inst_type}; found {len(cycles)}")
    return cycles[index - 1]


def normalize_pc_filter(value, pc_base):
    if value is None:
        return None
    if value < pc_base:
        return pc_base + value
    return value


def filter_rows(rows, cycle_from=None, cycle_to=None, pc_from=None, pc_to=None):
    use_pc_filter = pc_from is not None or pc_to is not None
    filtered = []
    for row in rows:
        cycle = row_cycle(row)
        if cycle is None:
            continue
        if cycle_from is not None and cycle < cycle_from:
            continue
        if cycle_to is not None and cycle > cycle_to:
            continue
        if use_pc_filter:
            pc = row_pc(row)
            if pc is None:
                continue
            if pc_from is not None and pc < pc_from:
                continue
            if pc_to is not None and pc > pc_to:
                continue
        filtered.append(row)
    return filtered


def write_event_markers(path, rows):
    fields = ["cycle", "wid", "pc", "inst_type", "policy"]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            if not row_issued(row):
                continue
            inst_type = row.get("inst_type", "")
            if inst_type not in EVENT_INST_TYPES:
                continue
            writer.writerow({
                "cycle": row.get("cycle", ""),
                "wid": row_actual_wid(row),
                "pc": row.get("pc", ""),
                "inst_type": inst_type,
                "policy": row.get("policy", ""),
            })


def event_cycles(rows, inst_type):
    cycles = []
    for row in rows:
        if not row_issued(row):
            continue
        if row.get("inst_type") != inst_type:
            continue
        cycle = row_cycle(row)
        if cycle is not None:
            cycles.append(cycle)
    return cycles


def write_split_index(path, splits):
    fields = ["split", "cycle_from", "cycle_to", "rows", "issued"]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for split in splits:
            writer.writerow(split)


def write_summary(
    path,
    rows,
    source_rows=None,
    cycle_from=None,
    cycle_to=None,
    pc_from=None,
    pc_to=None,
    pc_base=0,
):
    issued = [row for row in rows if row_issued(row)]
    all_cycles = sorted({cycle for cycle in (row_cycle(row) for row in rows) if cycle is not None})
    issue_cycles = sorted({cycle for cycle in (row_cycle(row) for row in issued) if cycle is not None})
    mismatches = [row for row in issued if row_mismatch(row)]
    fallbacks = [row for row in issued if row_fallback(row)]

    per_warp_count = defaultdict(int)
    per_warp_first = {}
    per_warp_last = {}
    per_warp_pcs = defaultdict(set)

    for row in issued:
        wid = row_actual_wid(row)
        cycle = row_cycle(row)
        if wid is None or wid < 0:
            continue
        per_warp_count[wid] += 1
        if cycle is not None:
            per_warp_first[wid] = min(per_warp_first.get(wid, cycle), cycle)
            per_warp_last[wid] = max(per_warp_last.get(wid, cycle), cycle)
        pc = row.get("pc", "")
        if pc != "":
            per_warp_pcs[wid].add(pc)

    total = len(issued)
    mismatch_count = len(mismatches)
    fallback_count = len(fallbacks)
    mismatch_rate = 100.0 * mismatch_count / total if total else 0.0
    fallback_rate = 100.0 * fallback_count / total if total else 0.0
    logged_span = (all_cycles[-1] - all_cycles[0] + 1) if all_cycles else 0

    with path.open("w") as f:
        if source_rows is not None and source_rows != len(rows):
            f.write(f"Source rows: {source_rows}\n")
            f.write(f"Filtered rows: {len(rows)}\n")
            f.write(f"Cycle filter from: {cycle_from if cycle_from is not None else 'begin'}\n")
            f.write(f"Cycle filter to: {cycle_to if cycle_to is not None else 'end'}\n")
        if pc_from is not None or pc_to is not None:
            pc_from_text = "begin" if pc_from is None else f"0x{pc_from:x} (offset 0x{pc_from - pc_base:x})"
            pc_to_text = "end" if pc_to is None else f"0x{pc_to:x} (offset 0x{pc_to - pc_base:x})"
            f.write(f"PC filter from: {pc_from_text}\n")
            f.write(f"PC filter to: {pc_to_text}\n")
        f.write(f"Total issued instructions: {total}\n")
        f.write(f"Total issue cycles: {len(issue_cycles)}\n")
        f.write(f"Total logged cycles: {len(all_cycles)}\n")
        f.write(f"Logged cycle span: {logged_span}\n")
        f.write(f"Mismatch count: {mismatch_count}\n")
        f.write(f"Mismatch rate: {mismatch_rate:.2f}%\n")
        f.write(f"Fallback count: {fallback_count}\n")
        f.write(f"Fallback rate: {fallback_rate:.2f}%\n\n")

        f.write("Per-warp issue count:\n")
        for wid in sorted(per_warp_count):
            f.write(f"WID {wid}: {per_warp_count[wid]}\n")

        f.write("\nPer-warp first issue cycle:\n")
        for wid in sorted(per_warp_count):
            f.write(f"WID {wid}: {per_warp_first.get(wid, 'n/a')}\n")

        f.write("\nPer-warp last issue cycle:\n")
        for wid in sorted(per_warp_count):
            f.write(f"WID {wid}: {per_warp_last.get(wid, 'n/a')}\n")

        f.write("\nPer-warp unique PC count:\n")
        for wid in sorted(per_warp_count):
            f.write(f"WID {wid}: {len(per_warp_pcs.get(wid, set()))}\n")


def write_mismatch_report(path, rows):
    mismatches = [row for row in rows if row_issued(row) and row_mismatch(row)]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=MISMATCH_FIELDS)
        writer.writeheader()
        for row in mismatches:
            out = {field: row.get(field, "") for field in MISMATCH_FIELDS}
            if out["actual_wid"] == "":
                out["actual_wid"] = row.get("selected_wid", "")
            if out["ibuffer_empty"] == "":
                out["ibuffer_empty"] = row.get("ibuffer_empty_mask", "")
            writer.writerow(out)


def import_matplotlib():
    os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "matplotlib"))
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.ticker import FuncFormatter

        return plt, FuncFormatter
    except ImportError as e:
        raise SystemExit(f"matplotlib is required for PNG output: {e}") from e


def issued_points(rows):
    points = []
    for row in rows:
        if not row_issued(row):
            continue
        cycle = row_cycle(row)
        wid = row_actual_wid(row)
        if cycle is None or wid is None or wid < 0:
            continue
        points.append((cycle, wid, row))
    return points


def plot_wid_timeline(path, rows):
    plt, _ = import_matplotlib()
    points = issued_points(rows)

    fig, ax = plt.subplots(figsize=(12, 4))
    if points:
        ax.scatter([p[0] for p in points], [p[1] for p in points], s=8)
    ax.set_xlabel("Cycle")
    ax.set_ylabel("Selected WID")
    ax.set_title("Warp Issue Timeline")
    ax.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)


def plot_pc_timeline(path, rows):
    plt, FuncFormatter = import_matplotlib()
    points = []
    for cycle, wid, row in issued_points(rows):
        pc = parse_int(row.get("pc"))
        if pc is not None:
            points.append((cycle, wid, pc))

    fig, ax = plt.subplots(figsize=(12, 5))
    if points:
        pc_base = min(pc for _, _, pc in points)
        markers = ["o", "s", "^", "D", "v", "P", "X", "*", "<", ">", "h", "8"]
        by_warp = defaultdict(list)
        for cycle, wid, pc in points:
            by_warp[wid].append((cycle, pc - pc_base))
        for index, wid in enumerate(sorted(by_warp)):
            xs = [x for x, _ in by_warp[wid]]
            ys = [y for _, y in by_warp[wid]]
            ax.scatter(xs, ys, s=10, marker=markers[index % len(markers)], label=f"WID {wid}")
        ax.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"0x{int(value):x}"))
        ax.set_ylabel(f"PC offset from 0x{pc_base:x}")
        ax.legend(loc="best", fontsize="small", ncols=2)
    else:
        ax.set_ylabel("PC offset")
    ax.set_xlabel("Cycle")
    ax.set_title("Per-Warp PC Timeline")
    ax.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)


def score_fields(fieldnames):
    fields = []
    if "score" in fieldnames:
        fields.append("score")
    fields.extend(sorted(
        (name for name in fieldnames if re.fullmatch(r"score_w\d+", name)),
        key=lambda name: int(name.removeprefix("score_w")),
    ))
    if "score_vector" in fieldnames:
        fields.append("score_vector")
    return fields


def has_score_data(rows, fields):
    for row in rows:
        for field in fields:
            if str(row.get(field, "")).strip() != "":
                return True
    return False


def parse_score_vector(value):
    if value is None or value == "":
        return []
    scores = []
    for item in value.split("|"):
        scores.append(parse_float(item))
    return scores


def plot_score_timeline(path, rows, fieldnames):
    fields = score_fields(fieldnames)
    if not fields or not has_score_data(rows, fields):
        return False

    plt, _ = import_matplotlib()
    points = issued_points(rows)
    fig, axes = plt.subplots(2, 1, figsize=(12, 6), sharex=True)

    if points:
        axes[0].scatter([p[0] for p in points], [p[1] for p in points], s=8)
    axes[0].set_ylabel("Selected WID")
    axes[0].grid(True, alpha=0.25)

    selected_scores = []
    for cycle, wid, row in points:
        score = parse_float(row.get("score"))
        if score is not None:
            selected_scores.append((cycle, score, wid))
    if selected_scores:
        axes[1].scatter(
            [x for x, _, _ in selected_scores],
            [y for _, y, _ in selected_scores],
            s=10,
            label="selected score",
        )

    per_warp_scores = defaultdict(list)
    for row in rows:
        cycle = row_cycle(row)
        if cycle is None:
            continue
        for field in fields:
            if re.fullmatch(r"score_w\d+", field):
                score = parse_float(row.get(field))
                if score is not None:
                    wid = int(field.removeprefix("score_w"))
                    per_warp_scores[wid].append((cycle, score))
        vector = parse_score_vector(row.get("score_vector"))
        for wid, score in enumerate(vector):
            if score is not None and not math.isnan(score):
                per_warp_scores[wid].append((cycle, score))

    for wid in sorted(per_warp_scores):
        xs = [x for x, _ in per_warp_scores[wid]]
        ys = [y for _, y in per_warp_scores[wid]]
        axes[1].plot(xs, ys, linewidth=0.8, alpha=0.55, label=f"WID {wid}")

    axes[1].set_xlabel("Cycle")
    axes[1].set_ylabel("Score")
    axes[1].grid(True, alpha=0.25)
    if selected_scores or per_warp_scores:
        axes[1].legend(loc="best", fontsize="small", ncols=2)
    fig.suptitle("Warp Scheduler Score Timeline")
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    return True


def analyze_rows(
    out_dir,
    rows,
    fieldnames,
    source_row_count=None,
    cycle_from=None,
    cycle_to=None,
    pc_from=None,
    pc_to=None,
    pc_base=0,
):
    out_dir.mkdir(parents=True, exist_ok=True)
    write_event_markers(out_dir / "event_markers.csv", rows)
    write_summary(
        out_dir / "summary.txt",
        rows,
        source_rows=source_row_count,
        cycle_from=cycle_from,
        cycle_to=cycle_to,
        pc_from=pc_from,
        pc_to=pc_to,
        pc_base=pc_base,
    )
    write_mismatch_report(out_dir / "mismatch_report.csv", rows)
    plot_wid_timeline(out_dir / "wid_timeline.png", rows)
    plot_pc_timeline(out_dir / "pc_timeline.png", rows)
    return plot_score_timeline(out_dir / "score_timeline.png", rows, fieldnames)


def main():
    parser = argparse.ArgumentParser(
        description="Analyze simx warp scheduler issue_trace.csv output."
    )
    parser.add_argument("issue_trace", type=Path, help="input issue_trace.csv")
    parser.add_argument(
        "-o",
        "--out-dir",
        type=Path,
        default=Path("."),
        help="directory for summary/report/PNG outputs",
    )
    parser.add_argument(
        "--cycle-from",
        type=parse_arg_int,
        default=None,
        help="include only rows at or after this cycle",
    )
    parser.add_argument(
        "--cycle-to",
        type=parse_arg_int,
        default=None,
        help="include only rows at or before this cycle",
    )
    parser.add_argument(
        "--pc-base",
        type=parse_arg_int,
        default=0x80000000,
        help="base PC used when --pc-from/--pc-to are given as offsets",
    )
    parser.add_argument(
        "--pc-from",
        type=parse_arg_int,
        default=None,
        help="include only issued rows at or after this PC; values below pc-base are treated as offsets",
    )
    parser.add_argument(
        "--pc-to",
        type=parse_arg_int,
        default=None,
        help="include only issued rows at or before this PC; values below pc-base are treated as offsets",
    )
    parser.add_argument(
        "--from-event",
        default=None,
        help="derive cycle-from from an issued event, e.g. WSPAWN:first, WSPAWN:3, BAR:first",
    )
    parser.add_argument(
        "--to-event",
        default=None,
        help="derive cycle-to from an issued event, e.g. TMC:last, FENCE:first, BAR:last",
    )
    parser.add_argument(
        "--split-by-wspawn",
        action="store_true",
        help="also analyze each interval from one issued WSPAWN to the cycle before the next WSPAWN",
    )
    args = parser.parse_args()

    rows, fieldnames = load_rows(args.issue_trace)
    source_row_count = len(rows)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    write_event_markers(args.out_dir / "event_markers_all.csv", rows)

    cycle_from = args.cycle_from
    cycle_to = args.cycle_to
    if args.from_event:
        cycle_from = select_event_cycle(rows, args.from_event)
    if args.to_event:
        cycle_to = select_event_cycle(rows, args.to_event)
    if cycle_from is not None or cycle_to is not None:
        rows = filter_rows(rows, cycle_from, cycle_to)

    pc_from = normalize_pc_filter(args.pc_from, args.pc_base)
    pc_to = normalize_pc_filter(args.pc_to, args.pc_base)
    if pc_from is not None or pc_to is not None:
        rows = filter_rows(rows, pc_from=pc_from, pc_to=pc_to)

    wrote_score = analyze_rows(
        args.out_dir,
        rows,
        fieldnames,
        source_row_count=source_row_count,
        cycle_from=cycle_from,
        cycle_to=cycle_to,
        pc_from=pc_from,
        pc_to=pc_to,
        pc_base=args.pc_base,
    )

    if args.split_by_wspawn:
        wspawn_cycles = event_cycles(rows, "WSPAWN")
        split_index = []
        for index, start_cycle in enumerate(wspawn_cycles):
            end_cycle = None
            if index + 1 < len(wspawn_cycles):
                end_cycle = wspawn_cycles[index + 1] - 1
            split_rows = filter_rows(rows, start_cycle, end_cycle)
            split_name = f"wspawn_{index + 1:02d}_{start_cycle}"
            split_dir = args.out_dir / split_name
            analyze_rows(
                split_dir,
                split_rows,
                fieldnames,
                source_row_count=source_row_count,
                cycle_from=start_cycle,
                cycle_to=end_cycle,
                pc_from=pc_from,
                pc_to=pc_to,
                pc_base=args.pc_base,
            )
            split_index.append({
                "split": split_name,
                "cycle_from": start_cycle,
                "cycle_to": end_cycle if end_cycle is not None else "end",
                "rows": len(split_rows),
                "issued": len([row for row in split_rows if row_issued(row)]),
            })
        write_split_index(args.out_dir / "wspawn_splits.csv", split_index)

    print(f"Wrote {args.out_dir / 'summary.txt'}")
    print(f"Wrote {args.out_dir / 'event_markers.csv'}")
    print(f"Wrote {args.out_dir / 'event_markers_all.csv'}")
    print(f"Wrote {args.out_dir / 'mismatch_report.csv'}")
    print(f"Wrote {args.out_dir / 'wid_timeline.png'}")
    print(f"Wrote {args.out_dir / 'pc_timeline.png'}")
    if wrote_score:
        print(f"Wrote {args.out_dir / 'score_timeline.png'}")
    else:
        print("Skipped score_timeline.png: no score data found", file=sys.stderr)
    if args.split_by_wspawn:
        print(f"Wrote {args.out_dir / 'wspawn_splits.csv'}")


if __name__ == "__main__":
    main()
