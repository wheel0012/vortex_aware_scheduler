#!/usr/bin/env python3
"""Plot 4-policy IPC and L1D MPKI normalized to RR for a single-kmeans
experiment dir (e.g. worklogs/experiments/kmeans_ver256_4lsu_4bank).

Usage:
    ./plot_kmeans_4policy.py <exp_dir>
    ./plot_kmeans_4policy.py worklogs/experiments/kmeans_ver256_4lsu_4bank
"""
import argparse, re, sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError as e:
    print(f"need matplotlib + numpy: {e}", file=sys.stderr)
    sys.exit(2)

POLICIES = ["RR", "GTO", "gCAWS", "iPAWS"]
COLORS = {"RR": "#a0a0a0", "GTO": "#4c8bf2", "gCAWS": "#f4a83d", "iPAWS": "#2bb673"}


def parse(log_path):
    txt = log_path.read_text(errors="ignore")
    m = re.search(r"^PERF: instrs=(\d+), cycles=(\d+), IPC=([0-9.]+)", txt, re.M)
    instrs = int(m.group(1)) if m else None
    ipc = float(m.group(3)) if m else None
    m = re.search(r"^PERF: core\d+: dcache read misses=(\d+) ", txt, re.M)
    rmiss = int(m.group(1)) if m else None
    mpki = (rmiss * 1000.0 / instrs) if (rmiss is not None and instrs) else None
    return {"ipc": ipc, "mpki": mpki, "rmiss": rmiss, "instrs": instrs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exp_dir")
    args = ap.parse_args()
    exp = Path(args.exp_dir).resolve()
    if not exp.is_dir():
        print(f"no dir: {exp}", file=sys.stderr)
        sys.exit(1)

    data = {}
    for pol in POLICIES:
        log = exp / pol / "kmeans.perf2.log"
        if not log.exists():
            print(f"missing: {log}", file=sys.stderr)
            sys.exit(1)
        data[pol] = parse(log)
        if data[pol]["ipc"] is None:
            print(f"could not parse IPC from {log}", file=sys.stderr)
            sys.exit(1)

    rr_ipc = data["RR"]["ipc"]
    rr_mpki = data["RR"]["mpki"]

    ipc_rel = [data[p]["ipc"] / rr_ipc for p in POLICIES]
    mpki_rel = [data[p]["mpki"] / rr_mpki if rr_mpki else 0 for p in POLICIES]

    # Plot
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.4))
    width = 0.55
    x = np.arange(len(POLICIES))

    # (a) IPC normalized
    bars1 = ax1.bar(x, ipc_rel, width, color=[COLORS[p] for p in POLICIES],
                    edgecolor="black", linewidth=0.5)
    ax1.axhline(1.0, color="black", linewidth=0.6, linestyle="--", alpha=0.5)
    ax1.set_xticks(x); ax1.set_xticklabels(POLICIES)
    ax1.set_ylabel("IPC normalized to RR")
    ax1.set_title("IPC Normalized to Baseline RR — kmeans")
    ax1.grid(axis="y", alpha=0.3)
    for b, v in zip(bars1, ipc_rel):
        ax1.text(b.get_x() + b.get_width() / 2, v + 0.005, f"{v:.3f}",
                 ha="center", va="bottom", fontsize=9)
    ymin = min(ipc_rel) * 0.97
    ymax = max(ipc_rel) * 1.04
    ax1.set_ylim(min(0.95, ymin), max(1.15, ymax))

    # (b) MPKI normalized
    bars2 = ax2.bar(x, mpki_rel, width, color=[COLORS[p] for p in POLICIES],
                    edgecolor="black", linewidth=0.5)
    ax2.axhline(1.0, color="black", linewidth=0.6, linestyle="--", alpha=0.5)
    ax2.set_xticks(x); ax2.set_xticklabels(POLICIES)
    ax2.set_ylabel("L1D MPKI normalized to RR")
    ax2.set_title("L1D cache MPKI Normalized to Baseline RR — kmeans")
    ax2.grid(axis="y", alpha=0.3)
    for b, v in zip(bars2, mpki_rel):
        ax2.text(b.get_x() + b.get_width() / 2, v + 0.01, f"{v:.3f}",
                 ha="center", va="bottom", fontsize=9)

    fig.tight_layout()
    out = exp / "ipc_mpki_vs_rr.png"
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")

    # Also a tiny table for reference
    print()
    print(f"{'Policy':<7} {'IPC':>10} {'Speedup_RR':>12} {'MPKI':>10} {'MPKI_RR':>10}")
    for p in POLICIES:
        ipc = data[p]["ipc"]; mpki = data[p]["mpki"]
        print(f"{p:<7} {ipc:>10.3f} {ipc/rr_ipc:>12.3f} {mpki:>10.2f} {mpki/rr_mpki:>10.3f}")


if __name__ == "__main__":
    main()
