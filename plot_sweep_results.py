#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Visualize Vortex simx sweep logs.

Usage:
  python3 plot_sweep_results.py sweep_results_20260511_123456

Optional:
  python3 plot_sweep_results.py sweep_results_20260511_123456 --normalize-to GTO

Outputs:
  <result_dir>/parsed_metrics.csv
  <result_dir>/plots/*.png
"""

import argparse
import csv
import re
from pathlib import Path
from typing import Dict, List, Optional

import matplotlib.pyplot as plt


# ============================================================
# Regex patterns
# ============================================================

GLOBAL_PATTERNS = {
    "ipc": re.compile(
        r"PERF:\s+instrs=(\d+),\s+cycles=(\d+),\s+IPC=([0-9.]+)"
    ),
    "memory_requests": re.compile(
        r"PERF:\s+memory requests=(\d+)\s+\(reads=(\d+),\s+writes=(\d+)\)"
    ),
    "memory_latency": re.compile(
        r"PERF:\s+memory latency=(\d+)\s+cycles"
    ),
    "scheduler_idle": re.compile(
        r"PERF:\s+scheduler idle=(\d+)\s+\((\d+)%\)"
    ),
    "ibuffer_stalls": re.compile(
        r"PERF:\s+ibuffer stalls=(\d+)\s+\((\d+)%\)"
    ),
    "scoreboard_stalls": re.compile(
        r"PERF:\s+scoreboard stalls=(\d+)\s+\((\d+)%\)"
        r"\s+\(alu=(\d+)%,\s+lsu=(\d+)%,\s+csrs=(\d+)%,\s+wctl=(\d+)%,\s+fpu=(\d+)%\)"
    ),
    "operands_stalls": re.compile(
        r"PERF:\s+operands stalls=(\d+)\s+\((\d+)%\)"
    ),
    "loads": re.compile(r"PERF:\s+loads=(\d+)"),
    "stores": re.compile(r"PERF:\s+stores=(\d+)"),
    "load_latency": re.compile(r"PERF:\s+load latency=(\d+)\s+cycles"),
    "ifetch_latency": re.compile(r"PERF:\s+ifetch latency=(\d+)\s+cycles"),
    "ccws_vta_inserts": re.compile(r"PERF:\s+ccws vta inserts=(\d+)"),
    "ccws_vta_hits": re.compile(r"PERF:\s+ccws vta hits=(\d+)"),
    "ccws_throttled_loads": re.compile(r"PERF:\s+ccws throttled loads=(\d+)"),
    "ccws_fallback_issues": re.compile(r"PERF:\s+ccws fallback issues=(\d+)"),
    "ccws_avg_candidates": re.compile(
        r"PERF:\s+ccws avg active issue candidates=([0-9.]+)"
    ),
    "ccws_avg_lls": re.compile(r"PERF:\s+ccws avg lls=([0-9.]+)"),
    "ccws_max_lls": re.compile(r"PERF:\s+ccws max lls=(\d+)"),
}

CORE_PATTERNS = {
    "dcache_reads": re.compile(r"PERF:\s+core\d+:\s+dcache reads=(\d+)"),
    "dcache_writes": re.compile(r"PERF:\s+core\d+:\s+dcache writes=(\d+)"),
    "dcache_read_misses": re.compile(
        r"PERF:\s+core\d+:\s+dcache read misses=(\d+)\s+\(hit ratio=(-?\d+)%\)"
    ),
    "dcache_write_misses": re.compile(
        r"PERF:\s+core\d+:\s+dcache write misses=(\d+)\s+\(hit ratio=(-?\d+)%\)"
    ),
    "dcache_bank_stalls": re.compile(
        r"PERF:\s+core\d+:\s+dcache bank stalls=(\d+)\s+\(utilization=(-?\d+)%\)"
    ),
    "dcache_mshr_stalls": re.compile(
        r"PERF:\s+core\d+:\s+dcache mshr stalls=(\d+)\s+\(utilization=(-?\d+)%\)"
    ),
    "coalescer_misses": re.compile(
        r"PERF:\s+core\d+:\s+coalescer misses=(\d+)\s+\(hit ratio=(-?\d+)%\)"
    ),
}


# ============================================================
# Parsing
# ============================================================

def parse_summary(summary_path: Path) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []

    with summary_path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)

    return rows


def parse_global_metrics(text: str) -> Dict[str, float]:
    """
    Parse global PERF lines.
    If multiple global lines appear, the last one overwrites previous values.
    """
    metrics: Dict[str, float] = {}

    for line in text.splitlines():
        m = GLOBAL_PATTERNS["ipc"].search(line)
        if m:
            metrics["instrs"] = float(m.group(1))
            metrics["cycles"] = float(m.group(2))
            metrics["ipc"] = float(m.group(3))
            continue

        m = GLOBAL_PATTERNS["memory_requests"].search(line)
        if m:
            metrics["memory_requests"] = float(m.group(1))
            metrics["memory_reads"] = float(m.group(2))
            metrics["memory_writes"] = float(m.group(3))
            continue

        m = GLOBAL_PATTERNS["memory_latency"].search(line)
        if m:
            metrics["memory_latency"] = float(m.group(1))
            continue

        m = GLOBAL_PATTERNS["scheduler_idle"].search(line)
        if m:
            metrics["scheduler_idle"] = float(m.group(1))
            metrics["scheduler_idle_pct"] = float(m.group(2))
            continue

        m = GLOBAL_PATTERNS["ibuffer_stalls"].search(line)
        if m:
            metrics["ibuffer_stalls"] = float(m.group(1))
            metrics["ibuffer_stalls_pct"] = float(m.group(2))
            continue

        m = GLOBAL_PATTERNS["scoreboard_stalls"].search(line)
        if m:
            metrics["scoreboard_stalls"] = float(m.group(1))
            metrics["scoreboard_stalls_pct"] = float(m.group(2))
            metrics["scoreboard_alu_pct"] = float(m.group(3))
            metrics["scoreboard_lsu_pct"] = float(m.group(4))
            metrics["scoreboard_csrs_pct"] = float(m.group(5))
            metrics["scoreboard_wctl_pct"] = float(m.group(6))
            metrics["scoreboard_fpu_pct"] = float(m.group(7))
            continue

        m = GLOBAL_PATTERNS["operands_stalls"].search(line)
        if m:
            metrics["operands_stalls"] = float(m.group(1))
            metrics["operands_stalls_pct"] = float(m.group(2))
            continue

        m = GLOBAL_PATTERNS["loads"].search(line)
        if m:
            metrics["loads"] = float(m.group(1))
            continue

        m = GLOBAL_PATTERNS["stores"].search(line)
        if m:
            metrics["stores"] = float(m.group(1))
            continue

        m = GLOBAL_PATTERNS["load_latency"].search(line)
        if m:
            metrics["load_latency"] = float(m.group(1))
            continue

        m = GLOBAL_PATTERNS["ifetch_latency"].search(line)
        if m:
            metrics["ifetch_latency"] = float(m.group(1))
            continue

        for key in [
            "ccws_vta_inserts",
            "ccws_vta_hits",
            "ccws_throttled_loads",
            "ccws_fallback_issues",
            "ccws_avg_candidates",
            "ccws_avg_lls",
            "ccws_max_lls",
        ]:
            m = GLOBAL_PATTERNS[key].search(line)
            if m:
                metrics[key] = float(m.group(1))
                break

    return metrics


def parse_core_aggregate_metrics(text: str) -> Dict[str, float]:
    """
    Sum per-core cache metrics.
    In multicore logs, Vortex also prints global metrics separately,
    but dcache metrics are usually per-core. We aggregate them.
    """
    metrics: Dict[str, float] = {
        "dcache_reads": 0.0,
        "dcache_writes": 0.0,
        "dcache_read_misses": 0.0,
        "dcache_write_misses": 0.0,
        "dcache_bank_stalls": 0.0,
        "dcache_mshr_stalls": 0.0,
        "coalescer_misses": 0.0,
    }

    for line in text.splitlines():
        for key, pattern in CORE_PATTERNS.items():
            m = pattern.search(line)
            if m:
                metrics[key] += float(m.group(1))

    return metrics


def add_derived_metrics(metrics: Dict[str, float]) -> Dict[str, float]:
    instrs = metrics.get("instrs", 0.0)
    loads = metrics.get("loads", 0.0)

    read_misses = metrics.get("dcache_read_misses", 0.0)
    write_misses = metrics.get("dcache_write_misses", 0.0)

    if instrs > 0:
        metrics["dcache_read_mpki"] = read_misses / instrs * 1000.0
        metrics["dcache_write_mpki"] = write_misses / instrs * 1000.0
        metrics["dcache_total_mpki"] = (read_misses + write_misses) / instrs * 1000.0

        metrics["memory_rpki"] = metrics.get("memory_reads", 0.0) / instrs * 1000.0
        metrics["memory_wpki"] = metrics.get("memory_writes", 0.0) / instrs * 1000.0
        metrics["memory_req_pki"] = metrics.get("memory_requests", 0.0) / instrs * 1000.0

        metrics["vta_hit_pki"] = metrics.get("ccws_vta_hits", 0.0) / instrs * 1000.0
        metrics["throttle_pki"] = metrics.get("ccws_throttled_loads", 0.0) / instrs * 1000.0
        metrics["fallback_pki"] = metrics.get("ccws_fallback_issues", 0.0) / instrs * 1000.0
    else:
        metrics["dcache_read_mpki"] = 0.0
        metrics["dcache_write_mpki"] = 0.0
        metrics["dcache_total_mpki"] = 0.0
        metrics["memory_rpki"] = 0.0
        metrics["memory_wpki"] = 0.0
        metrics["memory_req_pki"] = 0.0
        metrics["vta_hit_pki"] = 0.0
        metrics["throttle_pki"] = 0.0
        metrics["fallback_pki"] = 0.0

    inserts = metrics.get("ccws_vta_inserts", 0.0)
    hits = metrics.get("ccws_vta_hits", 0.0)
    throttled = metrics.get("ccws_throttled_loads", 0.0)
    fallback = metrics.get("ccws_fallback_issues", 0.0)

    metrics["ccws_vta_hit_rate"] = hits / inserts if inserts > 0 else 0.0
    metrics["ccws_throttle_rate"] = throttled / loads if loads > 0 else 0.0
    metrics["ccws_fallback_per_throttle"] = fallback / throttled if throttled > 0 else 0.0

    return metrics


def parse_log_file(log_path: Path) -> Dict[str, float]:
    text = log_path.read_text(errors="replace")

    metrics: Dict[str, float] = {}
    metrics.update(parse_core_aggregate_metrics(text))
    metrics.update(parse_global_metrics(text))
    metrics = add_derived_metrics(metrics)

    return metrics


def load_results(result_dir: Path) -> List[Dict[str, object]]:
    summary_path = result_dir / "summary.csv"
    if not summary_path.exists():
        raise FileNotFoundError(f"summary.csv not found: {summary_path}")

    summary_rows = parse_summary(summary_path)
    rows: List[Dict[str, object]] = []

    for row in summary_rows:
        log_path = Path(row["logfile"])

        if not log_path.is_absolute():
            # summary.csv may contain paths relative to cwd or result_dir.
            candidate1 = result_dir / log_path
            candidate2 = Path.cwd() / log_path

            if candidate1.exists():
                log_path = candidate1
            elif candidate2.exists():
                log_path = candidate2
            elif log_path.exists():
                pass
            else:
                print(f"[WARN] missing log: {row['logfile']}")
                continue

        metrics = parse_log_file(log_path)

        parsed: Dict[str, object] = {
            "benchmark": row["benchmark"],
            "policy": row["policy"],
            "config_name": row["config_name"],
            "cores": int(row["cores"]),
            "warps": int(row["warps"]),
            "threads": int(row["threads"]),
            "status": row["status"],
            "logfile": str(log_path),
        }
        parsed.update(metrics)
        rows.append(parsed)

    return rows


def write_parsed_csv(rows: List[Dict[str, object]], out_path: Path) -> None:
    if not rows:
        return

    keys = sorted({key for row in rows for key in row.keys()})

    with out_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


# ============================================================
# Normalization
# ============================================================

def label_of(row: Dict[str, object]) -> str:
    policy = str(row["policy"])
    config_name = str(row["config_name"])

    if policy == "CCWS":
        return config_name

    return policy


def add_normalized_metrics(rows: List[Dict[str, object]], normalize_to: str = "GTO") -> None:
    """
    Add metrics normalized to a baseline label per benchmark.
    Example:
      ipc_norm_to_GTO
      dcache_read_mpki_norm_to_GTO
    """
    metric_names = [
        "ipc",
        "cycles",
        "dcache_read_mpki",
        "dcache_write_mpki",
        "dcache_total_mpki",
        "memory_req_pki",
        "memory_rpki",
        "scoreboard_lsu_pct",
        "scheduler_idle_pct",
    ]

    by_bench: Dict[str, List[Dict[str, object]]] = {}
    for row in rows:
        if row.get("status") != "PASS":
            continue
        by_bench.setdefault(str(row["benchmark"]), []).append(row)

    for bench, bench_rows in by_bench.items():
        base_rows = [r for r in bench_rows if label_of(r) == normalize_to]

        if not base_rows:
            continue

        base = base_rows[0]

        for row in bench_rows:
            for metric in metric_names:
                key = f"{metric}_norm_to_{normalize_to}"
                base_value = float(base.get(metric, 0.0))
                value = float(row.get(metric, 0.0))

                if base_value == 0.0:
                    row[key] = 0.0
                else:
                    row[key] = value / base_value


# ============================================================
# Plotting
# ============================================================

def grouped_by_benchmark(rows: List[Dict[str, object]]) -> Dict[str, List[Dict[str, object]]]:
    groups: Dict[str, List[Dict[str, object]]] = {}

    for row in rows:
        if row.get("status") != "PASS":
            continue
        groups.setdefault(str(row["benchmark"]), []).append(row)

    return groups


def ordered_labels(rows: List[Dict[str, object]]) -> List[str]:
    preferred = [
        "STATIC",
        "RR",
        "GTO",
        "C0_vta8_lld10_p1_k4",
        "C1_vta16_lld64_p8_k4",
        "C2_vta16_lld128_p16_k2",
        "C3_vta32_lld128_p16_k1",
    ]

    present = {label_of(row) for row in rows}
    labels = [label for label in preferred if label in present]
    labels += sorted(present - set(labels))
    return labels


def plot_metric_by_benchmark(
    rows: List[Dict[str, object]],
    metric: str,
    ylabel: str,
    title: str,
    out_path: Path,
) -> None:
    groups = grouped_by_benchmark(rows)
    benchmarks = sorted(groups.keys())
    labels = ordered_labels(rows)

    labels = [
        label for label in labels
        if any(label_of(row) == label and metric in row for row in rows)
    ]

    if not benchmarks or not labels:
        print(f"[WARN] skip plot: {metric}")
        return

    x = list(range(len(benchmarks)))
    width = 0.8 / max(1, len(labels))

    plt.figure(figsize=(max(12, len(benchmarks) * 1.5), 5.5))

    for i, label in enumerate(labels):
        values = []

        for bench in benchmarks:
            candidates = [
                row for row in groups[bench]
                if label_of(row) == label and metric in row
            ]

            values.append(float(candidates[0][metric]) if candidates else 0.0)

        offsets = [
            xpos + (i - (len(labels) - 1) / 2.0) * width
            for xpos in x
        ]

        plt.bar(offsets, values, width=width, label=label)

    plt.xticks(x, benchmarks, rotation=30, ha="right")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.legend(fontsize=8)
    plt.tight_layout()
    plt.savefig(out_path, dpi=200)
    plt.close()


def plot_ccws_activity(rows: List[Dict[str, object]], out_path: Path) -> None:
    ccws_rows = [
        row for row in rows
        if row.get("status") == "PASS" and row.get("policy") == "CCWS"
    ]

    if not ccws_rows:
        print("[WARN] no CCWS rows")
        return

    labels = [
        f"{row['benchmark']}\n{row['config_name']}"
        for row in ccws_rows
    ]

    hits = [float(row.get("ccws_vta_hits", 0.0)) for row in ccws_rows]
    throttled = [float(row.get("ccws_throttled_loads", 0.0)) for row in ccws_rows]
    fallback = [float(row.get("ccws_fallback_issues", 0.0)) for row in ccws_rows]

    x = list(range(len(ccws_rows)))
    width = 0.25

    plt.figure(figsize=(max(14, len(ccws_rows) * 0.7), 5.5))
    plt.bar([i - width for i in x], hits, width=width, label="VTA hits")
    plt.bar(x, throttled, width=width, label="Throttled loads")
    plt.bar([i + width for i in x], fallback, width=width, label="Fallback issues")

    plt.xticks(x, labels, rotation=75, ha="right", fontsize=7)
    plt.ylabel("Count")
    plt.title("CCWS Activity Counters")
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_path, dpi=200)
    plt.close()


# ============================================================
# Main
# ============================================================

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("result_dir", type=str)
    parser.add_argument("--normalize-to", type=str, default="GTO")
    args = parser.parse_args()

    result_dir = Path(args.result_dir)
    plot_dir = result_dir / "plots"
    plot_dir.mkdir(exist_ok=True)

    rows = load_results(result_dir)
    add_normalized_metrics(rows, normalize_to=args.normalize_to)

    parsed_csv = result_dir / "parsed_metrics.csv"
    write_parsed_csv(rows, parsed_csv)

    print(f"[INFO] parsed CSV written: {parsed_csv}")

    # Raw performance
    plot_metric_by_benchmark(
        rows,
        "ipc",
        "IPC",
        "IPC by Benchmark and Policy",
        plot_dir / "ipc_by_benchmark.png",
    )

    plot_metric_by_benchmark(
        rows,
        "cycles",
        "Cycles",
        "Cycles by Benchmark and Policy",
        plot_dir / "cycles_by_benchmark.png",
    )

    # Raw MPKI / PKI
    plot_metric_by_benchmark(
        rows,
        "dcache_read_mpki",
        "D-cache Read MPKI",
        "D-cache Read MPKI by Benchmark and Policy",
        plot_dir / "dcache_read_mpki_by_benchmark.png",
    )

    plot_metric_by_benchmark(
        rows,
        "dcache_write_mpki",
        "D-cache Write MPKI",
        "D-cache Write MPKI by Benchmark and Policy",
        plot_dir / "dcache_write_mpki_by_benchmark.png",
    )

    plot_metric_by_benchmark(
        rows,
        "dcache_total_mpki",
        "D-cache Total MPKI",
        "D-cache Total MPKI by Benchmark and Policy",
        plot_dir / "dcache_total_mpki_by_benchmark.png",
    )

    plot_metric_by_benchmark(
        rows,
        "memory_req_pki",
        "Memory Request PKI",
        "Memory Request PKI by Benchmark and Policy",
        plot_dir / "memory_req_pki_by_benchmark.png",
    )

    # Normalized metrics
    norm = args.normalize_to

    plot_metric_by_benchmark(
        rows,
        f"ipc_norm_to_{norm}",
        f"IPC / {norm}",
        f"IPC Normalized to {norm}",
        plot_dir / f"ipc_norm_to_{norm}.png",
    )

    plot_metric_by_benchmark(
        rows,
        f"dcache_read_mpki_norm_to_{norm}",
        f"Read MPKI / {norm}",
        f"D-cache Read MPKI Normalized to {norm}",
        plot_dir / f"dcache_read_mpki_norm_to_{norm}.png",
    )

    plot_metric_by_benchmark(
        rows,
        f"memory_req_pki_norm_to_{norm}",
        f"Memory Req PKI / {norm}",
        f"Memory Request PKI Normalized to {norm}",
        plot_dir / f"memory_req_pki_norm_to_{norm}.png",
    )

    # Stall metrics
    plot_metric_by_benchmark(
        rows,
        "scheduler_idle_pct",
        "Scheduler Idle (%)",
        "Scheduler Idle Ratio by Benchmark and Policy",
        plot_dir / "scheduler_idle_by_benchmark.png",
    )

    plot_metric_by_benchmark(
        rows,
        "scoreboard_lsu_pct",
        "Scoreboard LSU Share (%)",
        "Scoreboard LSU Stall Share by Benchmark and Policy",
        plot_dir / "scoreboard_lsu_stall_by_benchmark.png",
    )

    # CCWS-specific derived metrics
    plot_metric_by_benchmark(
        rows,
        "ccws_vta_hit_rate",
        "VTA Hits / VTA Inserts",
        "CCWS VTA Hit Rate by Benchmark and Config",
        plot_dir / "ccws_vta_hit_rate_by_benchmark.png",
    )

    plot_metric_by_benchmark(
        rows,
        "ccws_throttle_rate",
        "Throttled Loads / Loads",
        "CCWS Throttle Rate by Benchmark and Config",
        plot_dir / "ccws_throttle_rate_by_benchmark.png",
    )

    plot_metric_by_benchmark(
        rows,
        "vta_hit_pki",
        "VTA Hit PKI",
        "CCWS VTA Hit PKI by Benchmark and Config",
        plot_dir / "ccws_vta_hit_pki_by_benchmark.png",
    )

    plot_metric_by_benchmark(
        rows,
        "throttle_pki",
        "Throttle PKI",
        "CCWS Throttle PKI by Benchmark and Config",
        plot_dir / "ccws_throttle_pki_by_benchmark.png",
    )

    plot_ccws_activity(rows, plot_dir / "ccws_activity_by_config.png")

    print(f"[INFO] plots written to: {plot_dir}")


if __name__ == "__main__":
    main()