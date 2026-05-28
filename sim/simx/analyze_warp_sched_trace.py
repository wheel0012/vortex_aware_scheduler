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
    "preferred_wid",
    "intended_wid",
    "actual_wid",
    "pc",
    "preferred_blocked",
    "preferred_block_reason",
    "not_ready_fallback",
    "mismatch_reason",
    "candidate_mask",
    "ready_mask",
    "ibuffer_empty",
    "fallback",
]

EVENT_INST_TYPES = {"WSPAWN", "TMC", "BAR", "FENCE", "CSRRS", "CSRRW"}
INTERACTIVE_BACKEND = False


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


def row_issue_slot(row):
    slot = parse_int(row.get("issue_slot"))
    return 0 if slot is None else slot


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


def row_preferred_blocked(row):
    return parse_bool(row.get("preferred_blocked", ""))


def row_not_ready_fallback(row):
    if row.get("not_ready_fallback", "") != "":
        return parse_bool(row.get("not_ready_fallback"))
    return row_preferred_blocked(row)


def load_rows(path):
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        fieldnames = reader.fieldnames or []
    return rows, fieldnames


def row_mask(row, key):
    value = parse_int(row.get(key))
    return 0 if value is None else value


def percent(value, total):
    return 100.0 * value / total if total else 0.0


def ratio_hit_percent(misses, accesses):
    return 100.0 * (accesses - misses) / accesses if accesses else 0.0


def metric_na():
    return None


def metric_row(name, value, unit="", source="trace", note=""):
    return {
        "metric": name,
        "value": "" if value is None else value,
        "unit": unit,
        "source": source,
        "note": note,
    }


def load_userpc_perf_log(path):
    metrics = {}
    if path is None or not path.exists():
        return metrics
    for line in path.read_text(errors="replace").splitlines():
        m = re.match(r"PERF: userpc scheduler idle=(\d+) \(([0-9.]+)%\)", line)
        if m:
            metrics["perf1.scheduler_idle"] = int(m.group(1))
            metrics["perf1.scheduler_idle_percent"] = m.group(2)
            continue
        m = re.match(r"PERF: userpc scheduler stalls=(\d+) \(([0-9.]+)%\)", line)
        if m:
            metrics["perf1.scheduler_stalls"] = int(m.group(1))
            metrics["perf1.scheduler_stalls_percent"] = m.group(2)
            continue
        m = re.match(r"PERF: userpc ibuffer stalls=(\d+) \(([0-9.]+)%\)", line)
        if m:
            metrics["perf1.ibuffer_stalls"] = int(m.group(1))
            metrics["perf1.ibuffer_stalls_percent"] = m.group(2)
            continue
        m = re.match(
            r"PERF: userpc scoreboard stalls=(\d+) \(([0-9.]+)%\) "
            r"\(alu=([0-9.]+)%, lsu=([0-9.]+)%, csrs=([0-9.]+)%, "
            r"wctl=([0-9.]+)%, fpu=([0-9.]+)%\)",
            line,
        )
        if m:
            metrics["perf1.scoreboard_stalls"] = int(m.group(1))
            metrics["perf1.scoreboard_stalls_percent"] = m.group(2)
            metrics["perf1.scoreboard_stalls.alu_percent"] = m.group(3)
            metrics["perf1.scoreboard_stalls.lsu_percent"] = m.group(4)
            metrics["perf1.scoreboard_stalls.csrs_percent"] = m.group(5)
            metrics["perf1.scoreboard_stalls.wctl_percent"] = m.group(6)
            metrics["perf1.scoreboard_stalls.fpu_percent"] = m.group(7)
            continue
        m = re.match(r"PERF: userpc ready checks=(\d+) / candidate checks=(\d+) \(hit ratio=([0-9.]+)%\)", line)
        if m:
            metrics["perf1.ready_checks"] = int(m.group(1))
            metrics["perf1.candidate_checks"] = int(m.group(2))
            metrics["perf1.ready_hit_ratio"] = m.group(3)
            continue
        m = re.match(r"PERF: userpc dcache requests=(\d+) \(reads=(\d+), writes=(\d+)\)", line)
        if m:
            metrics["perf2.dcache_requests"] = int(m.group(1))
            metrics["perf2.dcache_reads"] = int(m.group(2))
            metrics["perf2.dcache_writes"] = int(m.group(3))
            continue
        m = re.match(r"PERF: userpc dcache read latency=([0-9.]+) cycles", line)
        if m:
            metrics["perf2.dcache_read_latency"] = m.group(1)
            continue
        m = re.match(r"PERF: userpc memory requests=(\d+) \(reads=(\d+), writes=(\d+)\)", line)
        if m:
            metrics["perf2.memory_requests"] = int(m.group(1))
            metrics["perf2.memory_reads"] = int(m.group(2))
            metrics["perf2.memory_writes"] = int(m.group(3))
            continue
        m = re.match(r"PERF: userpc memory latency=([0-9.]+) cycles", line)
        if m:
            metrics["perf2.memory_latency"] = m.group(1)
            continue
    return metrics


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


def write_userpc_issue_trace(path, rows, pc_base):
    fields = [
        "userpc",
        "pc",
        "cycle",
        "core_id",
        "issue_slot",
        "selected_wid",
        "preferred_wid",
        "intended_wid",
        "inst_type",
        "score",
        "candidate_mask",
        "ready_mask",
        "ibuffer_empty_mask",
        "stall_reason",
        "preferred_blocked",
        "preferred_block_reason",
        "not_ready_fallback",
        "fallback",
        "mismatch",
        "mismatch_reason",
        "policy",
    ]
    issued = []
    for row in rows:
        if not row_issued(row):
            continue
        pc = row_pc(row)
        cycle = row_cycle(row)
        wid = row_actual_wid(row)
        if pc is None or cycle is None or wid is None:
            continue
        issued.append((pc - pc_base, cycle, row))

    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for userpc, cycle, row in sorted(
            issued,
            key=lambda item: (
                item[0],
                item[1],
                row_issue_slot(item[2]),
                row_actual_wid(item[2]) if row_actual_wid(item[2]) is not None else -1,
                item[2].get("inst_type", ""),
            ),
        ):
            pc = row_pc(row)
            writer.writerow({
                "userpc": f"0x{userpc:x}",
                "pc": f"0x{pc:x}" if pc is not None else "",
                "cycle": cycle,
                "core_id": row.get("core_id", ""),
                "issue_slot": row_issue_slot(row),
                "selected_wid": row_actual_wid(row),
                "preferred_wid": row.get("preferred_wid", ""),
                "intended_wid": row.get("intended_wid", ""),
                "inst_type": row.get("inst_type", ""),
                "score": row.get("score", ""),
                "candidate_mask": row.get("candidate_mask", ""),
                "ready_mask": row.get("ready_mask", ""),
                "ibuffer_empty_mask": row.get("ibuffer_empty_mask", ""),
                "stall_reason": row.get("stall_reason", ""),
                "preferred_blocked": row_preferred_blocked(row),
                "preferred_block_reason": row.get("preferred_block_reason", ""),
                "not_ready_fallback": row_not_ready_fallback(row),
                "fallback": row_fallback(row),
                "mismatch": row_mismatch(row),
                "mismatch_reason": row.get("mismatch_reason", ""),
                "policy": row.get("policy", ""),
            })


def write_userpc_summary(path, rows, pc_base):
    per_pc_count = defaultdict(int)
    per_pc_wids = defaultdict(lambda: defaultdict(int))
    per_pc_inst_types = defaultdict(set)
    per_pc_first = {}
    per_pc_last = {}

    for row in rows:
        if not row_issued(row):
            continue
        pc = row_pc(row)
        cycle = row_cycle(row)
        wid = row_actual_wid(row)
        if pc is None or cycle is None or wid is None:
            continue
        userpc = pc - pc_base
        per_pc_count[userpc] += 1
        per_pc_wids[userpc][wid] += 1
        inst_type = row.get("inst_type", "")
        if inst_type:
            per_pc_inst_types[userpc].add(inst_type)
        per_pc_first[userpc] = min(per_pc_first.get(userpc, cycle), cycle)
        per_pc_last[userpc] = max(per_pc_last.get(userpc, cycle), cycle)

    fields = [
        "userpc",
        "pc",
        "issues",
        "first_cycle",
        "last_cycle",
        "wids",
        "inst_types",
    ]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for userpc in sorted(per_pc_count):
            wid_counts = per_pc_wids[userpc]
            writer.writerow({
                "userpc": f"0x{userpc:x}",
                "pc": f"0x{pc_base + userpc:x}",
                "issues": per_pc_count[userpc],
                "first_cycle": per_pc_first[userpc],
                "last_cycle": per_pc_last[userpc],
                "wids": "|".join(f"{wid}:{wid_counts[wid]}" for wid in sorted(wid_counts)),
                "inst_types": "|".join(sorted(per_pc_inst_types[userpc])),
            })


def mask_to_wids(value):
    mask = parse_int(value)
    if mask is None:
        return ""
    return "|".join(str(wid) for wid in range(mask.bit_length()) if mask & (1 << wid))


def format_userpc(row, pc_base):
    pc = row_pc(row)
    if pc is None:
        return ""
    return f"0x{pc - pc_base:x}"


def write_cycle_issue_trace(path, rows, pc_base):
    fields = [
        "cycle",
        "core_id",
        "issue_slot",
        "issued",
        "selected_wid",
        "selected_userpc",
        "selected_pc",
        "inst_type",
        "preferred_wid",
        "preferred_ready",
        "preferred_blocked",
        "preferred_block_reason",
        "not_ready_fallback",
        "intended_wid",
        "intended_ready",
        "candidate_wids",
        "ready_wids",
        "ibuffer_empty_wids",
        "score",
        "score_vector",
        "stall_reason",
        "mismatch",
        "mismatch_reason",
        "fallback",
        "policy",
    ]
    ordered = []
    for index, row in enumerate(rows):
        cycle = row_cycle(row)
        if cycle is None:
            continue
        ordered.append((cycle, row_issue_slot(row), index, row))

    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for _, _, _, row in sorted(ordered):
            selected_wid = row_actual_wid(row)
            preferred_wid = parse_int(row.get("preferred_wid"))
            intended_wid = parse_int(row.get("intended_wid"))
            ready_mask = parse_int(row.get("ready_mask"))
            preferred_ready = ""
            if preferred_wid is not None and preferred_wid >= 0 and ready_mask is not None:
                preferred_ready = bool(ready_mask & (1 << preferred_wid))
            intended_ready = ""
            if intended_wid is not None and intended_wid >= 0 and ready_mask is not None:
                intended_ready = bool(ready_mask & (1 << intended_wid))
            pc = row_pc(row)
            writer.writerow({
                "cycle": row.get("cycle", ""),
                "core_id": row.get("core_id", ""),
                "issue_slot": row_issue_slot(row),
                "issued": row_issued(row),
                "selected_wid": selected_wid if selected_wid is not None else "",
                "selected_userpc": format_userpc(row, pc_base),
                "selected_pc": f"0x{pc:x}" if pc is not None else "",
                "inst_type": row.get("inst_type", ""),
                "preferred_wid": row.get("preferred_wid", ""),
                "preferred_ready": preferred_ready,
                "preferred_blocked": row_preferred_blocked(row),
                "preferred_block_reason": row.get("preferred_block_reason", ""),
                "not_ready_fallback": row_not_ready_fallback(row),
                "intended_wid": row.get("intended_wid", ""),
                "intended_ready": intended_ready,
                "candidate_wids": mask_to_wids(row.get("candidate_mask")),
                "ready_wids": mask_to_wids(row.get("ready_mask")),
                "ibuffer_empty_wids": mask_to_wids(row.get("ibuffer_empty_mask")),
                "score": row.get("score", ""),
                "score_vector": row.get("score_vector", ""),
                "stall_reason": row.get("stall_reason", ""),
                "mismatch": row_mismatch(row),
                "mismatch_reason": row.get("mismatch_reason", ""),
                "fallback": row_fallback(row),
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
    preferred_blocked = [row for row in issued if row_preferred_blocked(row)]
    not_ready_fallbacks = [row for row in issued if row_not_ready_fallback(row)]

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
    preferred_blocked_count = len(preferred_blocked)
    not_ready_fallback_count = len(not_ready_fallbacks)
    mismatch_rate = 100.0 * mismatch_count / total if total else 0.0
    fallback_rate = 100.0 * fallback_count / total if total else 0.0
    preferred_blocked_rate = 100.0 * preferred_blocked_count / total if total else 0.0
    not_ready_fallback_rate = 100.0 * not_ready_fallback_count / total if total else 0.0
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
        f.write(f"Not-ready fallback count: {not_ready_fallback_count}\n")
        f.write(f"Not-ready fallback rate: {not_ready_fallback_rate:.2f}%\n")
        f.write(f"Preferred blocked count: {preferred_blocked_count}\n")
        f.write(f"Preferred blocked rate: {preferred_blocked_rate:.2f}%\n")
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


def build_perf_metrics(rows, perf_window_rows=None, external_metrics=None):
    external_metrics = external_metrics or {}
    issued = [row for row in rows if row_issued(row)]
    issued_cycles = [row_cycle(row) for row in issued if row_cycle(row) is not None]
    if perf_window_rows is None:
        perf_window_rows = rows
    window_cycles = [row_cycle(row) for row in perf_window_rows if row_cycle(row) is not None]
    if issued_cycles:
        first_cycle = min(issued_cycles)
        last_cycle = max(issued_cycles)
        span_cycles = last_cycle - first_cycle + 1
    elif window_cycles:
        first_cycle = min(window_cycles)
        last_cycle = max(window_cycles)
        span_cycles = last_cycle - first_cycle + 1
    else:
        first_cycle = None
        last_cycle = None
        span_cycles = 0

    slot_rows = len(perf_window_rows)
    slot_issued = len([row for row in perf_window_rows if row_issued(row)])
    scheduler_idle = len([
        row for row in perf_window_rows
        if not row_issued(row) and row_mask(row, "candidate_mask") == 0
    ])
    scoreboard_stalls = len([
        row for row in perf_window_rows
        if not row_issued(row)
        and row_mask(row, "candidate_mask") != 0
        and row_mask(row, "ready_mask") == 0
    ])
    scheduler_other_stalls = max(0, slot_rows - scheduler_idle - scoreboard_stalls - slot_issued)

    loads = len([row for row in issued if row.get("inst_type") in {"LOAD"}])
    stores = len([row for row in issued if row.get("inst_type") in {"STORE"}])
    instrs = len(issued)
    ipc = (instrs / span_cycles) if span_cycles else 0.0
    issue_density = (instrs / span_cycles) if span_cycles else 0.0
    issue_active_rate = (instrs / len(set(issued_cycles))) if issued_cycles else 0.0

    metrics = [
        metric_row("pc_window.first_cycle", first_cycle, "cycles", "trace"),
        metric_row("pc_window.last_cycle", last_cycle, "cycles", "trace"),
        metric_row("pc_window.span_cycles", span_cycles, "cycles", "trace"),
        metric_row("pc_window.issue_cycles", len(set(issued_cycles)), "cycles", "trace"),
        metric_row("pc_window.slot_rows", slot_rows, "slot-cycles", "trace",
                   "scheduler rows in first..last selected-PC issue span"),
        metric_row("pc_window.issue_density", f"{issue_density:.6f}", "issued/span_cycle", "trace"),
        metric_row("pc_window.issue_active_rate", f"{issue_active_rate:.6f}", "issued/active_issue_cycle", "trace"),

        metric_row("perf1.scheduler_idle", external_metrics.get("perf1.scheduler_idle", scheduler_idle), "cycles",
                   "simx-run-log" if "perf1.scheduler_idle" in external_metrics else "trace"),
        metric_row("perf1.scheduler_idle_percent", external_metrics.get("perf1.scheduler_idle_percent", f"{percent(scheduler_idle, slot_rows):.2f}"), "%",
                   "simx-run-log" if "perf1.scheduler_idle_percent" in external_metrics else "trace"),
        metric_row("perf1.scheduler_stalls", external_metrics.get("perf1.scheduler_stalls", scheduler_other_stalls), "cycles",
                   "simx-run-log" if "perf1.scheduler_stalls" in external_metrics else "trace-derived",
                   "non-idle, non-scoreboard, non-issued scheduler rows; not identical to simx CSR"),
        metric_row("perf1.scheduler_stalls_percent", external_metrics.get("perf1.scheduler_stalls_percent", f"{percent(scheduler_other_stalls, slot_rows):.2f}"), "%",
                   "simx-run-log" if "perf1.scheduler_stalls_percent" in external_metrics else "trace-derived"),
        metric_row("perf1.ibuffer_stalls", external_metrics.get("perf1.ibuffer_stalls"), "cycles",
                   "simx-run-log" if "perf1.ibuffer_stalls" in external_metrics else "unavailable",
                   "decode ibuffer-full stalls are only available from simx run.log"),
        metric_row("perf1.ibuffer_stalls_percent", external_metrics.get("perf1.ibuffer_stalls_percent"), "%",
                   "simx-run-log" if "perf1.ibuffer_stalls_percent" in external_metrics else "unavailable"),
        metric_row("perf1.scoreboard_stalls", external_metrics.get("perf1.scoreboard_stalls", scoreboard_stalls), "cycles",
                   "simx-run-log" if "perf1.scoreboard_stalls" in external_metrics else "trace"),
        metric_row("perf1.scoreboard_stalls_percent", external_metrics.get("perf1.scoreboard_stalls_percent", f"{percent(scoreboard_stalls, slot_rows):.2f}"), "%",
                   "simx-run-log" if "perf1.scoreboard_stalls_percent" in external_metrics else "trace"),
        metric_row("perf1.scoreboard_stalls.alu_percent", external_metrics.get("perf1.scoreboard_stalls.alu_percent"), "%",
                   "simx-run-log" if "perf1.scoreboard_stalls.alu_percent" in external_metrics else "unavailable",
                   "blocked dependency FU type is only available from simx run.log"),
        metric_row("perf1.scoreboard_stalls.lsu_percent", external_metrics.get("perf1.scoreboard_stalls.lsu_percent"), "%",
                   "simx-run-log" if "perf1.scoreboard_stalls.lsu_percent" in external_metrics else "unavailable"),
        metric_row("perf1.scoreboard_stalls.csrs_percent", external_metrics.get("perf1.scoreboard_stalls.csrs_percent"), "%",
                   "simx-run-log" if "perf1.scoreboard_stalls.csrs_percent" in external_metrics else "unavailable"),
        metric_row("perf1.scoreboard_stalls.wctl_percent", external_metrics.get("perf1.scoreboard_stalls.wctl_percent"), "%",
                   "simx-run-log" if "perf1.scoreboard_stalls.wctl_percent" in external_metrics else "unavailable"),
        metric_row("perf1.scoreboard_stalls.fpu_percent", external_metrics.get("perf1.scoreboard_stalls.fpu_percent"), "%",
                   "simx-run-log" if "perf1.scoreboard_stalls.fpu_percent" in external_metrics else "unavailable"),
        metric_row("perf1.ready_checks", external_metrics.get("perf1.ready_checks"), "checks",
                   "simx-run-log" if "perf1.ready_checks" in external_metrics else "unavailable"),
        metric_row("perf1.candidate_checks", external_metrics.get("perf1.candidate_checks"), "checks",
                   "simx-run-log" if "perf1.candidate_checks" in external_metrics else "unavailable"),
        metric_row("perf1.ready_hit_ratio", external_metrics.get("perf1.ready_hit_ratio"), "%",
                   "simx-run-log" if "perf1.ready_hit_ratio" in external_metrics else "unavailable"),
        metric_row("perf1.operands_stalls", metric_na(), "cycles", "unavailable",
                   "operand-stage stalls are not present in issue_trace.csv"),
        metric_row("perf1.ifetches", instrs, "instructions", "estimated",
                   "one fetch per selected issued instruction"),
        metric_row("perf1.loads", loads, "instructions", "trace"),
        metric_row("perf1.stores", stores, "instructions", "trace"),
        metric_row("perf1.ifetch_latency", metric_na(), "cycles", "unavailable"),
        metric_row("perf1.load_latency", metric_na(), "cycles", "unavailable"),
        metric_row("perf.instrs", instrs, "instructions", "trace",
                   "issue-trace instruction count, not MINSTRET thread-mask count"),
        metric_row("perf.cycles", span_cycles, "cycles", "trace",
                   "first selected-PC issue through last selected-PC issue"),
        metric_row("perf.IPC", f"{ipc:.6f}", "instrs/cycle", "trace-derived"),

        metric_row("perf2.lmem_reads", metric_na(), "requests", "unavailable"),
        metric_row("perf2.lmem_writes", metric_na(), "requests", "unavailable"),
        metric_row("perf2.lmem_bank_stalls", metric_na(), "cycles", "unavailable"),
        metric_row("perf2.icache_reads", instrs, "requests", "estimated",
                   "approximated as selected-PC ifetches"),
        metric_row("perf2.icache_read_misses", metric_na(), "requests", "unavailable"),
        metric_row("perf2.icache_read_hit_ratio", metric_na(), "%", "unavailable"),
        metric_row("perf2.icache_mshr_stalls", metric_na(), "cycles", "unavailable"),
        metric_row("perf2.dcache_requests", external_metrics.get("perf2.dcache_requests"), "requests", "simx-run-log",
                   "userpc-marked dcache transactions from simx run.log"),
        metric_row("perf2.dcache_reads", external_metrics.get("perf2.dcache_reads", loads), "requests",
                   "simx-run-log" if "perf2.dcache_reads" in external_metrics else "estimated",
                   "simx dcache transactions when run.log is provided; otherwise instruction-level load count"),
        metric_row("perf2.dcache_writes", external_metrics.get("perf2.dcache_writes", stores), "requests",
                   "simx-run-log" if "perf2.dcache_writes" in external_metrics else "estimated",
                   "simx dcache transactions when run.log is provided; otherwise instruction-level store count"),
        metric_row("perf2.dcache_read_misses", metric_na(), "requests", "unavailable"),
        metric_row("perf2.dcache_read_hit_ratio", metric_na(), "%", "unavailable"),
        metric_row("perf2.dcache_write_misses", metric_na(), "requests", "unavailable"),
        metric_row("perf2.dcache_write_hit_ratio", metric_na(), "%", "unavailable"),
        metric_row("perf2.dcache_bank_stalls", metric_na(), "cycles", "unavailable"),
        metric_row("perf2.dcache_mshr_stalls", metric_na(), "cycles", "unavailable"),
        metric_row("perf2.dcache_read_latency", external_metrics.get("perf2.dcache_read_latency"), "cycles", "simx-run-log",
                   "average userpc dcache read latency from simx run.log"),
        metric_row("perf2.coalescer_misses", metric_na(), "requests", "unavailable"),
        metric_row("perf2.coalescer_hit_ratio", metric_na(), "%", "unavailable"),
        metric_row("perf2.l2cache_reads", metric_na(), "requests", "unavailable"),
        metric_row("perf2.l2cache_writes", metric_na(), "requests", "unavailable"),
        metric_row("perf2.l2cache_read_misses", metric_na(), "requests", "unavailable"),
        metric_row("perf2.l2cache_read_hit_ratio", metric_na(), "%", "unavailable"),
        metric_row("perf2.l2cache_write_misses", metric_na(), "requests", "unavailable"),
        metric_row("perf2.l2cache_write_hit_ratio", metric_na(), "%", "unavailable"),
        metric_row("perf2.l2cache_bank_stalls", metric_na(), "cycles", "unavailable"),
        metric_row("perf2.l2cache_mshr_stalls", metric_na(), "cycles", "unavailable"),
        metric_row("perf2.l3cache_reads", metric_na(), "requests", "unavailable"),
        metric_row("perf2.l3cache_writes", metric_na(), "requests", "unavailable"),
        metric_row("perf2.l3cache_read_misses", metric_na(), "requests", "unavailable"),
        metric_row("perf2.l3cache_read_hit_ratio", metric_na(), "%", "unavailable"),
        metric_row("perf2.l3cache_write_misses", metric_na(), "requests", "unavailable"),
        metric_row("perf2.l3cache_write_hit_ratio", metric_na(), "%", "unavailable"),
        metric_row("perf2.l3cache_bank_stalls", metric_na(), "cycles", "unavailable"),
        metric_row("perf2.l3cache_mshr_stalls", metric_na(), "cycles", "unavailable"),
        metric_row("perf2.memory_requests", external_metrics.get("perf2.memory_requests"), "requests", "simx-run-log"),
        metric_row("perf2.memory_reads", external_metrics.get("perf2.memory_reads"), "requests", "simx-run-log"),
        metric_row("perf2.memory_writes", external_metrics.get("perf2.memory_writes"), "requests", "simx-run-log"),
        metric_row("perf2.memory_latency", external_metrics.get("perf2.memory_latency"), "cycles", "simx-run-log"),
        metric_row("perf2.memory_bank_stalls", metric_na(), "cycles", "unavailable"),
    ]
    return metrics


def write_perf_metrics_csv(path, metrics):
    fields = ["metric", "value", "unit", "source", "note"]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(metrics)


def write_perf_summary(path, metrics):
    values = {metric["metric"]: metric for metric in metrics}

    def val(name):
        value = values.get(name, {}).get("value", "")
        return "n/a" if value == "" else value

    with path.open("w") as f:
        f.write("# PC-filtered issue-trace perf summary\n")
        f.write("# Values marked unavailable are not present in issue_trace.csv.\n")
        f.write("# Instruction counts are issue-trace instruction counts, not MINSTRET thread-mask counts.\n\n")

        f.write(f"PERF: analyzer pc_window first_cycle={val('pc_window.first_cycle')}, "
                f"last_cycle={val('pc_window.last_cycle')}, "
                f"span_cycles={val('pc_window.span_cycles')}, "
                f"issue_cycles={val('pc_window.issue_cycles')}, "
                f"issue_density={val('pc_window.issue_density')}, "
                f"issue_active_rate={val('pc_window.issue_active_rate')}\n")
        f.write(f"PERF: scheduler idle={val('perf1.scheduler_idle')} "
                f"({val('perf1.scheduler_idle_percent')}%)\n")
        f.write(f"PERF: scheduler stalls={val('perf1.scheduler_stalls')} "
                f"({val('perf1.scheduler_stalls_percent')}%)\n")
        f.write(f"PERF: ibuffer stalls={val('perf1.ibuffer_stalls')}\n")
        f.write(f"PERF: scoreboard stalls={val('perf1.scoreboard_stalls')} "
                f"({val('perf1.scoreboard_stalls_percent')}%) "
                f"(alu={val('perf1.scoreboard_stalls.alu_percent')}%, "
                f"lsu={val('perf1.scoreboard_stalls.lsu_percent')}%, "
                f"csrs={val('perf1.scoreboard_stalls.csrs_percent')}%, "
                f"wctl={val('perf1.scoreboard_stalls.wctl_percent')}%, "
                f"fpu={val('perf1.scoreboard_stalls.fpu_percent')}%)\n")
        f.write(f"PERF: operands stalls={val('perf1.operands_stalls')}\n")
        f.write(f"PERF: ifetches={val('perf1.ifetches')}\n")
        f.write(f"PERF: loads={val('perf1.loads')}\n")
        f.write(f"PERF: stores={val('perf1.stores')}\n")
        f.write(f"PERF: ifetch latency={val('perf1.ifetch_latency')} cycles\n")
        f.write(f"PERF: load latency={val('perf1.load_latency')} cycles\n")

        f.write(f"PERF: lmem reads={val('perf2.lmem_reads')}\n")
        f.write(f"PERF: lmem writes={val('perf2.lmem_writes')}\n")
        f.write(f"PERF: lmem bank stalls={val('perf2.lmem_bank_stalls')}\n")
        f.write(f"PERF: icache reads={val('perf2.icache_reads')}\n")
        f.write(f"PERF: icache read misses={val('perf2.icache_read_misses')} "
                f"(hit ratio={val('perf2.icache_read_hit_ratio')}%)\n")
        f.write(f"PERF: icache mshr stalls={val('perf2.icache_mshr_stalls')}\n")
        f.write(f"PERF: dcache reads={val('perf2.dcache_reads')}\n")
        f.write(f"PERF: dcache writes={val('perf2.dcache_writes')}\n")
        f.write(f"PERF: dcache requests={val('perf2.dcache_requests')} "
                f"(reads={val('perf2.dcache_reads')}, writes={val('perf2.dcache_writes')})\n")
        f.write(f"PERF: dcache read misses={val('perf2.dcache_read_misses')} "
                f"(hit ratio={val('perf2.dcache_read_hit_ratio')}%)\n")
        f.write(f"PERF: dcache write misses={val('perf2.dcache_write_misses')} "
                f"(hit ratio={val('perf2.dcache_write_hit_ratio')}%)\n")
        f.write(f"PERF: dcache bank stalls={val('perf2.dcache_bank_stalls')}\n")
        f.write(f"PERF: dcache mshr stalls={val('perf2.dcache_mshr_stalls')}\n")
        f.write(f"PERF: dcache read latency={val('perf2.dcache_read_latency')} cycles\n")
        f.write(f"PERF: coalescer misses={val('perf2.coalescer_misses')} "
                f"(hit ratio={val('perf2.coalescer_hit_ratio')}%)\n")
        f.write(f"PERF: l2cache reads={val('perf2.l2cache_reads')}\n")
        f.write(f"PERF: l2cache writes={val('perf2.l2cache_writes')}\n")
        f.write(f"PERF: l2cache read misses={val('perf2.l2cache_read_misses')} "
                f"(hit ratio={val('perf2.l2cache_read_hit_ratio')}%)\n")
        f.write(f"PERF: l2cache write misses={val('perf2.l2cache_write_misses')} "
                f"(hit ratio={val('perf2.l2cache_write_hit_ratio')}%)\n")
        f.write(f"PERF: l2cache bank stalls={val('perf2.l2cache_bank_stalls')}\n")
        f.write(f"PERF: l2cache mshr stalls={val('perf2.l2cache_mshr_stalls')}\n")
        f.write(f"PERF: l3cache reads={val('perf2.l3cache_reads')}\n")
        f.write(f"PERF: l3cache writes={val('perf2.l3cache_writes')}\n")
        f.write(f"PERF: l3cache read misses={val('perf2.l3cache_read_misses')} "
                f"(hit ratio={val('perf2.l3cache_read_hit_ratio')}%)\n")
        f.write(f"PERF: l3cache write misses={val('perf2.l3cache_write_misses')} "
                f"(hit ratio={val('perf2.l3cache_write_hit_ratio')}%)\n")
        f.write(f"PERF: l3cache bank stalls={val('perf2.l3cache_bank_stalls')}\n")
        f.write(f"PERF: l3cache mshr stalls={val('perf2.l3cache_mshr_stalls')}\n")
        f.write(f"PERF: memory requests={val('perf2.memory_requests')} "
                f"(reads={val('perf2.memory_reads')}, writes={val('perf2.memory_writes')})\n")
        f.write(f"PERF: memory latency={val('perf2.memory_latency')} cycles\n")
        f.write(f"PERF: memory bank stalls={val('perf2.memory_bank_stalls')}\n")
        f.write(f"PERF: instrs={val('perf.instrs')}, cycles={val('perf.cycles')}, IPC={val('perf.IPC')}\n")


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


def write_preferred_blocked_report(path, rows):
    blocked = [row for row in rows if row_issued(row) and row_not_ready_fallback(row)]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=MISMATCH_FIELDS)
        writer.writeheader()
        for row in blocked:
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

        if not INTERACTIVE_BACKEND:
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


def plot_width(points, min_width=12.0, max_width=48.0):
    if not points:
        return min_width
    cycles = [point[0] for point in points]
    span = max(cycles) - min(cycles) + 1
    by_span = span / 700.0
    by_points = len(points) / 2500.0
    return min(max_width, max(min_width, by_span, by_points))


def set_integer_wid_ticks(ax, points):
    if not points:
        return
    wids = sorted({point[1] for point in points})
    if len(wids) <= 32:
        ax.set_yticks(wids)


def plot_wid_timeline(path, rows, show=False):
    plt, _ = import_matplotlib()
    points = issued_points(rows)
    width = plot_width(points)
    by_slot = defaultdict(list)
    for point in points:
        by_slot[row_issue_slot(point[2])].append(point)

    slots = sorted(by_slot) or [0]
    height = max(4.0, 1.8 * len(slots) + 1.2)
    fig, axes = plt.subplots(len(slots), 1, figsize=(width, height), sharex=True, squeeze=False)
    axes = [row[0] for row in axes]

    for ax, slot in zip(axes, slots):
        slot_points = sorted(by_slot.get(slot, []), key=lambda point: (point[0], point[1]))
        if slot_points:
            xs = [point[0] for point in slot_points]
            ys = [point[1] for point in slot_points]
            ax.step(xs, ys, where="post", linewidth=0.8, alpha=0.55)
            ax.scatter(xs, ys, s=12, alpha=0.85)

            mismatch_points = [point for point in slot_points if row_mismatch(point[2])]
            if mismatch_points:
                ax.scatter(
                    [point[0] for point in mismatch_points],
                    [point[1] for point in mismatch_points],
                    s=32,
                    facecolors="none",
                    edgecolors="tab:red",
                    linewidths=0.9,
                    label="mismatch",
                )

            not_ready_points = [point for point in slot_points if row_not_ready_fallback(point[2])]
            if not_ready_points:
                ax.scatter(
                    [point[0] for point in not_ready_points],
                    [point[1] for point in not_ready_points],
                    s=42,
                    marker="x",
                    color="tab:red",
                    linewidths=1.2,
                    label="not-ready fallback selected",
                )
                preferred_points = [
                    (point[0], parse_int(point[2].get("preferred_wid")))
                    for point in not_ready_points
                ]
                preferred_points = [
                    (cycle, wid)
                    for cycle, wid in preferred_points
                    if wid is not None and wid >= 0
                ]
                if preferred_points:
                    ax.scatter(
                        [cycle for cycle, _ in preferred_points],
                        [wid for _, wid in preferred_points],
                        s=48,
                        marker="D",
                        facecolors="none",
                        edgecolors="tab:orange",
                        linewidths=1.0,
                        label="blocked preferred",
                    )
            if mismatch_points or not_ready_points:
                ax.legend(loc="upper right", fontsize="small")
            set_integer_wid_ticks(ax, slot_points)

        ax.set_ylabel(f"Slot {slot}\nSelected WID")
        ax.grid(True, alpha=0.25)

    axes[-1].set_xlabel("Cycle")
    fig.suptitle("Warp Issue Timeline")
    fig.tight_layout()
    if path is not None:
        fig.savefig(path, dpi=160)
    if show:
        plt.show()
    plt.close(fig)


def plot_pc_timeline(path, rows):
    plt, FuncFormatter = import_matplotlib()
    points = []
    for cycle, wid, row in issued_points(rows):
        pc = parse_int(row.get("pc"))
        if pc is not None:
            points.append((cycle, wid, pc))

    fig, ax = plt.subplots(figsize=(plot_width(points), 5))
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
    fig, axes = plt.subplots(2, 1, figsize=(plot_width(points), 6), sharex=True)

    if points:
        axes[0].scatter([p[0] for p in points], [p[1] for p in points], s=8)
        not_ready_points = [point for point in points if row_not_ready_fallback(point[2])]
        if not_ready_points:
            axes[0].scatter(
                [point[0] for point in not_ready_points],
                [point[1] for point in not_ready_points],
                s=38,
                marker="x",
                color="tab:red",
                linewidths=1.1,
                label="not-ready fallback",
            )
            axes[0].legend(loc="upper right", fontsize="small")
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
    perf_window_rows=None,
    source_row_count=None,
    cycle_from=None,
    cycle_to=None,
    pc_from=None,
    pc_to=None,
    pc_base=0,
    external_metrics=None,
    write_plots=True,
    write_heavy_csv=True,
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
    if write_heavy_csv:
        write_mismatch_report(out_dir / "mismatch_report.csv", rows)
        write_preferred_blocked_report(out_dir / "preferred_blocked_report.csv", rows)
        write_preferred_blocked_report(out_dir / "not_ready_fallback_report.csv", rows)
        write_userpc_issue_trace(out_dir / "userpc_issue_trace.csv", rows, pc_base)
    write_userpc_summary(out_dir / "userpc_summary.csv", rows, pc_base)
    if write_heavy_csv:
        write_cycle_issue_trace(out_dir / "cycle_issue_trace.csv", rows, pc_base)
    perf_metrics = build_perf_metrics(rows, perf_window_rows, external_metrics)
    write_perf_metrics_csv(out_dir / "perf_metrics.csv", perf_metrics)
    write_perf_summary(out_dir / "perf_summary.txt", perf_metrics)
    if not write_plots:
        return False
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
    parser.add_argument(
        "--show-wid-timeline",
        action="store_true",
        help="open an interactive WID timeline window after writing analysis outputs",
    )
    parser.add_argument(
        "--no-plots",
        action="store_true",
        help="skip PNG timeline generation; useful for large traces when only CSV/text metrics are needed",
    )
    parser.add_argument(
        "--lite",
        action="store_true",
        help="skip large derived CSV reports; keep summary, userpc_summary, and perf metrics",
    )
    parser.add_argument(
        "--run-log",
        type=Path,
        default=None,
        help="optional simx run.log; fills userpc dcache/memory metrics emitted by simx",
    )
    args = parser.parse_args()

    global INTERACTIVE_BACKEND
    INTERACTIVE_BACKEND = args.show_wid_timeline

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
    cycle_filtered_rows = rows

    pc_from = normalize_pc_filter(args.pc_from, args.pc_base)
    pc_to = normalize_pc_filter(args.pc_to, args.pc_base)
    if pc_from is not None or pc_to is not None:
        rows = filter_rows(rows, pc_from=pc_from, pc_to=pc_to)
    perf_window_rows = cycle_filtered_rows
    issued_cycles = [row_cycle(row) for row in rows if row_issued(row) and row_cycle(row) is not None]
    if issued_cycles:
        perf_window_rows = filter_rows(cycle_filtered_rows, min(issued_cycles), max(issued_cycles))
    external_metrics = load_userpc_perf_log(args.run_log)

    wrote_score = analyze_rows(
        args.out_dir,
        rows,
        fieldnames,
        perf_window_rows=perf_window_rows,
        source_row_count=source_row_count,
        cycle_from=cycle_from,
        cycle_to=cycle_to,
        pc_from=pc_from,
        pc_to=pc_to,
        pc_base=args.pc_base,
        external_metrics=external_metrics,
        write_plots=not args.no_plots,
        write_heavy_csv=not args.lite,
    )

    if args.split_by_wspawn:
        wspawn_cycles = event_cycles(rows, "WSPAWN")
        split_index = []
        for index, start_cycle in enumerate(wspawn_cycles):
            end_cycle = None
            if index + 1 < len(wspawn_cycles):
                end_cycle = wspawn_cycles[index + 1] - 1
            split_rows = filter_rows(rows, start_cycle, end_cycle)
            split_window_rows = filter_rows(perf_window_rows, start_cycle, end_cycle)
            split_name = f"wspawn_{index + 1:02d}_{start_cycle}"
            split_dir = args.out_dir / split_name
            analyze_rows(
                split_dir,
                split_rows,
                fieldnames,
                perf_window_rows=split_window_rows,
                source_row_count=source_row_count,
                cycle_from=start_cycle,
                cycle_to=end_cycle,
                pc_from=pc_from,
                pc_to=pc_to,
                pc_base=args.pc_base,
                external_metrics=external_metrics,
                write_plots=not args.no_plots,
                write_heavy_csv=not args.lite,
            )
            split_index.append({
                "split": split_name,
                "cycle_from": start_cycle,
                "cycle_to": end_cycle if end_cycle is not None else "end",
                "rows": len(split_rows),
                "issued": len([row for row in split_rows if row_issued(row)]),
            })
        write_split_index(args.out_dir / "wspawn_splits.csv", split_index)

    if args.show_wid_timeline:
        plot_wid_timeline(None, rows, show=True)

    print(f"Wrote {args.out_dir / 'summary.txt'}")
    print(f"Wrote {args.out_dir / 'event_markers.csv'}")
    print(f"Wrote {args.out_dir / 'event_markers_all.csv'}")
    if args.lite:
        print("Skipped large derived CSV reports (--lite)")
    else:
        print(f"Wrote {args.out_dir / 'mismatch_report.csv'}")
        print(f"Wrote {args.out_dir / 'preferred_blocked_report.csv'}")
        print(f"Wrote {args.out_dir / 'not_ready_fallback_report.csv'}")
        print(f"Wrote {args.out_dir / 'userpc_issue_trace.csv'}")
    print(f"Wrote {args.out_dir / 'userpc_summary.csv'}")
    if not args.lite:
        print(f"Wrote {args.out_dir / 'cycle_issue_trace.csv'}")
    print(f"Wrote {args.out_dir / 'perf_summary.txt'}")
    print(f"Wrote {args.out_dir / 'perf_metrics.csv'}")
    if args.no_plots:
        print("Skipped timeline PNGs (--no-plots)")
    else:
        print(f"Wrote {args.out_dir / 'wid_timeline.png'}")
        print(f"Wrote {args.out_dir / 'pc_timeline.png'}")
        if wrote_score:
            print(f"Wrote {args.out_dir / 'score_timeline.png'}")
        else:
            print("Skipped score_timeline.png: no score data found", file=sys.stderr)
    if args.split_by_wspawn:
        print(f"Wrote {args.out_dir / 'wspawn_splits.csv'}")
    if args.show_wid_timeline:
        print("Displayed interactive WID timeline")


if __name__ == "__main__":
    main()
