#!/usr/bin/env python3
"""Plot kmeans 4-policy Speedup_RR and MPKI_RR vs input size.
Reads worklogs/experiments/kmeans_inputsweep_*/p<N>/<pol>/kmeans.perf2.log.

Usage: ./plot_kmeans_inputsweep.py <exp_dir>
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
MARKERS = {"RR": "o", "GTO": "s", "gCAWS": "D", "iPAWS": "^"}


def parse(log_path):
    txt = log_path.read_text(errors="ignore")
    m = re.search(r"^PERF: instrs=(\d+), cycles=(\d+), IPC=([0-9.]+)", txt, re.M)
    instrs = int(m.group(1)) if m else None
    ipc = float(m.group(3)) if m else None
    m = re.search(r"^PERF: core\d+: dcache read misses=(\d+) ", txt, re.M)
    rmiss = int(m.group(1)) if m else None
    mpki = (rmiss * 1000.0 / instrs) if (rmiss and instrs) else None
    return {"ipc": ipc, "mpki": mpki, "rmiss": rmiss, "instrs": instrs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exp_dir")
    args = ap.parse_args()
    exp = Path(args.exp_dir).resolve()
    if not exp.is_dir():
        print(f"no dir: {exp}", file=sys.stderr); sys.exit(1)

    # Discover -p<N> subdirs
    p_dirs = sorted([d for d in exp.iterdir() if d.is_dir() and d.name.startswith("p")],
                    key=lambda d: int(d.name[1:]))
    if not p_dirs:
        print("no p<N>/ subdirs found", file=sys.stderr); sys.exit(1)
    p_sizes = [int(d.name[1:]) for d in p_dirs]
    print(f"input sizes: {p_sizes}")

    data = {pol: {} for pol in POLICIES}
    for p_dir, p in zip(p_dirs, p_sizes):
        for pol in POLICIES:
            log = p_dir / pol / "kmeans.perf2.log"
            if not log.exists():
                print(f"missing: {log}", file=sys.stderr); continue
            data[pol][p] = parse(log)

    # Per p, compute speedup and mpki ratio relative to RR
    ipc_rel = {pol: [] for pol in POLICIES}
    mpki_rel = {pol: [] for pol in POLICIES}
    for p in p_sizes:
        rr_ipc = data["RR"][p]["ipc"]
        rr_mpki = data["RR"][p]["mpki"]
        for pol in POLICIES:
            ipc_rel[pol].append(data[pol][p]["ipc"] / rr_ipc)
            mpki_rel[pol].append(data[pol][p]["mpki"] / rr_mpki if rr_mpki else 0)

    # Plot two side-by-side line charts
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4.6))

    x = np.arange(len(p_sizes))
    xlabels = [f"-p{p}\n({p*100*4//1024} KB)" for p in p_sizes]

    for pol in POLICIES:
        ax1.plot(x, ipc_rel[pol], marker=MARKERS[pol], color=COLORS[pol],
                 label=pol, linewidth=1.6, markersize=8)
    ax1.axhline(1.0, color="black", linewidth=0.6, linestyle="--", alpha=0.5)
    ax1.set_xticks(x); ax1.set_xticklabels(xlabels, fontsize=9)
    ax1.set_xlabel("kmeans input size (-p <npoints>, working set)")
    ax1.set_ylabel("IPC normalized to RR")
    ax1.set_title("IPC Normalized to Baseline RR — kmeans (input-size sweep)")
    ax1.legend(loc="best", fontsize=9)
    ax1.grid(True, alpha=0.3)
    # Annotate gCAWS values
    for xi, v in zip(x, ipc_rel["gCAWS"]):
        ax1.text(xi, v + 0.01, f"{v:.3f}", ha="center", fontsize=8,
                 color=COLORS["gCAWS"])

    for pol in POLICIES:
        ax2.plot(x, mpki_rel[pol], marker=MARKERS[pol], color=COLORS[pol],
                 label=pol, linewidth=1.6, markersize=8)
    ax2.axhline(1.0, color="black", linewidth=0.6, linestyle="--", alpha=0.5)
    ax2.set_xticks(x); ax2.set_xticklabels(xlabels, fontsize=9)
    ax2.set_xlabel("kmeans input size (-p <npoints>, working set)")
    ax2.set_ylabel("L1D MPKI normalized to RR")
    ax2.set_title("L1D cache MPKI Normalized to Baseline RR — kmeans (input-size sweep)")
    ax2.legend(loc="best", fontsize=9)
    ax2.grid(True, alpha=0.3)

    fig.tight_layout()
    out = exp / "ipc_mpki_inputsweep.png"
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")

    # Print summary table
    print()
    print(f"{'p':>6} | " + " | ".join(f"{pol:>14}" for pol in POLICIES))
    print("-" * 80)
    for i, p in enumerate(p_sizes):
        ipc_strs = " | ".join(f"{ipc_rel[pol][i]:.3f}({data[pol][p]['ipc']:.2f})" for pol in POLICIES)
        print(f"{p:>6} | {ipc_strs}")


if __name__ == "__main__":
    main()
