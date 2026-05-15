#!/usr/bin/env python3
"""run4 plotter: per-workload IPC + L1D MPKI bars normalized to RR.

Collects perf=2 logs from worklogs/experiments/{kmeans_ver256_4lsu_4bank,
hotspot_wg256_4lsu_4bank, sgemm3_wg1024_4lsu_4bank} and writes:
  worklogs/runs/run4/<workload>_ipc_mpki_vs_rr.png

Some logs (sgemm3 GTO) contain two concatenated runs — we always take the
FIRST PERF block so we measure the intended -n96 invocation.
"""
import re, sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
EXPS = {
    "kmeans":  ROOT / "experiments/kmeans_ver256_4lsu_4bank",
    "hotspot": ROOT / "experiments/hotspot_wg256_4lsu_4bank",
    "sgemm3":  ROOT / "experiments/sgemm3_wg1024_4lsu_4bank",
    "bfs":     ROOT / "experiments/bfs_wg256_4lsu_4bank/g128k",
    "streamcluster":     ROOT / "experiments/streamcluster_large_4lsu_4bank"
}
POLICIES = ["RR", "GTO", "gCAWS", "iPAWS"]
COLORS = {"RR": "#a0a0a0", "GTO": "#4c8bf2", "gCAWS": "#f4a83d", "iPAWS": "#2bb673"}
OUT = ROOT / "runs/run4"


def parse_first(log_path):
    """Take the FIRST PERF block in the log (defends against duplicated runs)."""
    txt = log_path.read_text(errors="ignore")
    m = re.search(r"^PERF: instrs=(\d+), cycles=(\d+), IPC=([0-9.]+)", txt, re.M)
    if not m:
        return None
    instrs, cycles, ipc = int(m.group(1)), int(m.group(2)), float(m.group(3))
    m = re.search(r"^PERF: core\d+: dcache read misses=(\d+) \(hit ratio=(\d+)%\)",
                  txt, re.M)
    rmiss, l1hit = (int(m.group(1)), int(m.group(2))) if m else (None, None)
    m = re.search(r"^PERF: core\d+: dcache reads=(\d+)", txt, re.M)
    dreads = int(m.group(1)) if m else None
    m = re.search(r"^PERF: core\d+: dcache writes=(\d+)", txt, re.M)
    dwrites = int(m.group(1)) if m else None
    m = re.search(r"^PERF: core\d+: dcache write misses=(\d+)", txt, re.M)
    wmiss = int(m.group(1)) if m else None
    m = re.search(r"^PERF: core\d+: dcache bank stalls=\d+ \(stall rate=(\d+)%\)",
                  txt, re.M)
    bank_stall = int(m.group(1)) if m else None
    m = re.search(r"^PERF: core\d+: dcache mshr stalls=\d+ \(stall rate=(\d+)%\)",
                  txt, re.M)
    mshr_stall = int(m.group(1)) if m else None
    m = re.search(r"^PERF: core\d+: coalescer misses=\d+ \(split rate=(\d+)%\)",
                  txt, re.M)
    split = int(m.group(1)) if m else None
    mpki = (rmiss * 1000.0 / instrs) if rmiss is not None and instrs else 0.0
    return dict(instrs=instrs, cycles=cycles, ipc=ipc, rmiss=rmiss, wmiss=wmiss,
                mpki=mpki, l1hit=l1hit, dreads=dreads, dwrites=dwrites,
                bank_stall=bank_stall, mshr_stall=mshr_stall, split=split)


def parse_perf1(log_path):
    txt = log_path.read_text(errors="ignore")
    m = re.search(r"^PERF: instrs=(\d+), cycles=(\d+), IPC=([0-9.]+)", txt, re.M)
    if not m:
        return None
    out = dict(instrs=int(m.group(1)), cycles=int(m.group(2)),
               ipc=float(m.group(3)))
    m = re.search(r"^PERF: scheduler idle=\d+ \((\d+)%\)", txt, re.M)
    out["sched_idle"] = int(m.group(1)) if m else None
    m = re.search(r"^PERF: ibuffer stalls=\d+ \((\d+)%\)", txt, re.M)
    out["ibuf"] = int(m.group(1)) if m else None
    m = re.search(r"^PERF: scoreboard stalls=\d+ \((\d+)%\) \(alu=\d+%, lsu=(\d+)%",
                  txt, re.M)
    out["sb_lsu"] = int(m.group(2)) if m else None
    return out


def plot_one(bench, data, outfile):
    rr = data["RR"]
    ipc_rel = [data[p]["perf2"]["ipc"] / rr["perf2"]["ipc"] for p in POLICIES]
    rr_mpki = rr["perf2"]["mpki"] or 1.0
    mpki_rel = [(data[p]["perf2"]["mpki"] / rr_mpki) if rr_mpki else 0
                for p in POLICIES]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.4))
    width = 0.55
    x = np.arange(len(POLICIES))

    bars1 = ax1.bar(x, ipc_rel, width,
                    color=[COLORS[p] for p in POLICIES],
                    edgecolor="black", linewidth=0.5)
    ax1.axhline(1.0, color="black", linewidth=0.6, linestyle="--", alpha=0.5)
    ax1.set_xticks(x); ax1.set_xticklabels(POLICIES)
    ax1.set_ylabel("IPC normalized to RR")
    ax1.set_title(f"IPC Normalized to Baseline RR — {bench}")
    ax1.grid(axis="y", alpha=0.3)
    for b, v in zip(bars1, ipc_rel):
        ax1.text(b.get_x() + b.get_width()/2, v + 0.003, f"{v:.3f}",
                 ha="center", va="bottom", fontsize=9)
    ymin = min(0.95, min(ipc_rel) * 0.97)
    ymax = max(1.15, max(ipc_rel) * 1.04)
    ax1.set_ylim(ymin, ymax)

    bars2 = ax2.bar(x, mpki_rel, width,
                    color=[COLORS[p] for p in POLICIES],
                    edgecolor="black", linewidth=0.5)
    ax2.axhline(1.0, color="black", linewidth=0.6, linestyle="--", alpha=0.5)
    ax2.set_xticks(x); ax2.set_xticklabels(POLICIES)
    ax2.set_ylabel("L1D MPKI normalized to RR")
    ax2.set_title(f"L1D MPKI Normalized to Baseline RR — {bench}")
    ax2.grid(axis="y", alpha=0.3)
    for b, v in zip(bars2, mpki_rel):
        ax2.text(b.get_x() + b.get_width()/2, v + 0.01, f"{v:.3f}",
                 ha="center", va="bottom", fontsize=9)

    fig.tight_layout()
    fig.savefig(outfile, dpi=150)
    print(f"wrote {outfile}")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    all_data = {}
    for bench, exp in EXPS.items():
        d = {}
        for pol in POLICIES:
            p1 = parse_perf1(exp / pol / f"{bench}.perf1.log")
            p2 = parse_first(exp / pol / f"{bench}.perf2.log")
            if p2 is None:
                print(f"missing perf2 for {bench}/{pol}", file=sys.stderr)
                sys.exit(1)
            d[pol] = {"perf1": p1, "perf2": p2}
        all_data[bench] = d
        plot_one(bench, d, OUT / f"{bench}_ipc_mpki_vs_rr.png")
    return all_data


if __name__ == "__main__":
    main()
