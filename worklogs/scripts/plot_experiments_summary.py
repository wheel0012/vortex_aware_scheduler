#!/usr/bin/env python3
"""Plot normalized IPC and L1D total MPKI for selected experiments.

The bars are normalized to each benchmark's RR baseline and use the policy
order/colors requested for the experiments summary.

Output:
    worklogs/experiments/experiments_normalized_ipc_l1d_mpki_vs_rr.png
"""
import argparse
import re
import sys
from pathlib import Path

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError as e:
    print(f"need matplotlib + numpy: {e}", file=sys.stderr)
    sys.exit(2)


ROOT = Path(__file__).resolve().parent.parent
POLICIES = ["RR", "GTO", "gCAWS", "iPAWS"]
COLORS = {"RR": "#a0a0a0", "GTO": "#4c8bf2", "gCAWS": "#f4a83d", "iPAWS": "#2bb673"}

BENCHMARKS = [
    {
        "label": "bfs",
        "log_name": "bfs",
        "dirs": [ROOT / "experiments/bfs_wg256_4lsu_4bank/g128k"],
    },
    {
        "label": "hotspot",
        "log_name": "hotspot",
        "dirs": [ROOT / "experiments/hotspot_wg256_4lsu_4bank"],
    },
    {
        "label": "kmeans",
        "log_name": "kmeans",
        "dirs": [
            ROOT / "experiments/kmeans_ver256_4lsu_4bank",
            ROOT / "experiments/kmeans_ver256",
        ],
    },
    {
        "label": "sgemm3",
        "log_name": "sgemm3",
        "dirs": [ROOT / "experiments/sgemm3_wg1024_4lsu_4bank"],
    },
    {
        "label": "streamcluster",
        "log_name": "streamcluster",
        "dirs": [ROOT / "experiments/streamcluster_large_4lsu_4bank"],
    },
]


def first_existing_dir(candidates):
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return candidates[0]


def parse_perf2(log_path):
    txt = log_path.read_text(errors="ignore")

    m = re.search(r"^PERF: instrs=(\d+), cycles=(\d+), IPC=([0-9.]+)", txt, re.M)
    if not m:
        return None
    instrs = int(m.group(1))
    ipc = float(m.group(3))

    m = re.search(r"^PERF: core\d+: dcache read misses=(\d+) ", txt, re.M)
    read_misses = int(m.group(1)) if m else None

    m = re.search(r"^PERF: core\d+: dcache write misses=(\d+) ", txt, re.M)
    write_misses = int(m.group(1)) if m else None

    if read_misses is None or write_misses is None or not instrs:
        total_misses = None
        mpki = None
    else:
        total_misses = read_misses + write_misses
        mpki = total_misses * 1000.0 / instrs

    return {
        "instrs": instrs,
        "ipc": ipc,
        "read_misses": read_misses,
        "write_misses": write_misses,
        "total_misses": total_misses,
        "mpki": mpki,
    }


def collect():
    data = {}
    for bench in BENCHMARKS:
        label = bench["label"]
        exp_dir = first_existing_dir(bench["dirs"])
        data[label] = {}
        for policy in POLICIES:
            log_path = exp_dir / policy / f"{bench['log_name']}.perf2.log"
            if not log_path.exists():
                print(f"missing: {log_path}", file=sys.stderr)
                sys.exit(1)
            stats = parse_perf2(log_path)
            if stats is None or stats["ipc"] is None or stats["mpki"] is None:
                print(f"could not parse IPC/L1D MPKI from {log_path}", file=sys.stderr)
                sys.exit(1)
            data[label][policy] = stats
    return data


def normalized_values(data, metric):
    values = {policy: [] for policy in POLICIES}
    for bench in data:
        baseline = data[bench]["RR"][metric]
        if not baseline:
            print(f"RR baseline for {bench}/{metric} is zero or missing", file=sys.stderr)
            sys.exit(1)
        for policy in POLICIES:
            values[policy].append(data[bench][policy][metric] / baseline)
    return values


def annotate_bars(ax, bars, values):
    for bar, value in zip(bars, values):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 0.015,
            f"{value:.2f}",
            ha="center",
            va="bottom",
            fontsize=7,
            rotation=90,
        )


def plot(data, outfile):
    benches = list(data.keys())
    x = np.arange(len(benches))
    width = 0.19

    ipc_rel = normalized_values(data, "ipc")
    mpki_rel = normalized_values(data, "mpki")

    fig, (ax_ipc, ax_mpki) = plt.subplots(2, 1, figsize=(11, 7.2), sharex=True)

    for i, policy in enumerate(POLICIES):
        offset = (i - (len(POLICIES) - 1) / 2) * width
        ipc_bars = ax_ipc.bar(
            x + offset,
            ipc_rel[policy],
            width,
            label=policy,
            color=COLORS[policy],
            edgecolor="black",
            linewidth=0.4,
        )
        mpki_bars = ax_mpki.bar(
            x + offset,
            mpki_rel[policy],
            width,
            label=policy,
            color=COLORS[policy],
            edgecolor="black",
            linewidth=0.4,
        )
        annotate_bars(ax_ipc, ipc_bars, ipc_rel[policy])
        annotate_bars(ax_mpki, mpki_bars, mpki_rel[policy])

    for ax in (ax_ipc, ax_mpki):
        ax.axhline(1.0, color="black", linewidth=0.6, linestyle="--", alpha=0.55)
        ax.grid(axis="y", alpha=0.3)
        ax.set_axisbelow(True)
        ax.legend(loc="upper center", ncol=len(POLICIES), frameon=False)

    ax_ipc.set_ylabel("Normalized IPC")
    ax_ipc.set_title("IPC normalized to baseline RR")
    ax_ipc.set_xticks(x)
    ax_ipc.set_xticklabels(benches, rotation=15, ha="right")
    ax_ipc.tick_params(axis="x", labelbottom=True)
    ax_mpki.set_ylabel("Normalized L1D total MPKI")
    ax_mpki.set_title("L1D total MPKI normalized to baseline RR")
    ax_mpki.set_xticks(x)
    ax_mpki.set_xticklabels(benches, rotation=15, ha="right")

    for ax, rel in ((ax_ipc, ipc_rel), (ax_mpki, mpki_rel)):
        max_value = max(max(values) for values in rel.values())
        ax.set_ylim(0.94, max(1.1, max_value * 1.08))

    fig.tight_layout()
    outfile.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(outfile, dpi=150)
    print(f"wrote {outfile}")


def print_table(data):
    print()
    print(f"{'Benchmark':<16} {'Policy':<7} {'IPC_RR':>8} {'L1D_TOTAL_MPKI_RR':>18}")
    for bench, by_policy in data.items():
        rr_ipc = by_policy["RR"]["ipc"]
        rr_mpki = by_policy["RR"]["mpki"]
        for policy in POLICIES:
            ipc_rel = by_policy[policy]["ipc"] / rr_ipc
            mpki_rel = by_policy[policy]["mpki"] / rr_mpki if rr_mpki else 0.0
            print(f"{bench:<16} {policy:<7} {ipc_rel:>8.3f} {mpki_rel:>18.3f}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=ROOT / "experiments/experiments_normalized_ipc_l1d_mpki_vs_rr.png",
        help="output PNG path",
    )
    args = parser.parse_args()

    data = collect()
    plot(data, args.output)
    print_table(data)


if __name__ == "__main__":
    main()
