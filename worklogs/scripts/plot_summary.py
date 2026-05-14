#!/usr/bin/env python3
"""Parse perf=2 logs from a runN/ directory and plot CAWA-style bars:
   (a) IPC normalized to RR baseline (Fig 9 style)
   (b) MPKI normalized to RR baseline (Fig 10 style)

Usage: ./plot_summary.py [<runN_dir>]
       defaults to the highest-numbered runN/ under worklogs/runs/.
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

POLICIES = ["GTO", "RR", "gCAWS", "iPAWS"]
COLORS = {"GTO": "#4c8bf2", "RR": "#a0a0a0", "gCAWS": "#f4a83d", "iPAWS": "#2bb673"}


def parse_log(log_path):
    if not log_path.exists():
        return None
    txt = log_path.read_text(errors="ignore")
    instrs = cycles = ipc = rmiss = None
    m = re.search(r"^PERF: instrs=(\d+), cycles=(\d+), IPC=([0-9.]+)", txt, re.M)
    if m:
        instrs, cycles, ipc = int(m.group(1)), int(m.group(2)), float(m.group(3))
    m = re.search(r"^PERF: core\d+: dcache read misses=(\d+) ", txt, re.M)
    if m:
        rmiss = int(m.group(1))
    return {"instrs": instrs, "cycles": cycles, "ipc": ipc, "rmiss": rmiss}


def collect(run_dir, benches):
    data = {}
    for pol in POLICIES:
        for bench in benches:
            log = run_dir / pol / f"{bench}.perf2.log"
            stats = parse_log(log)
            if stats and stats["ipc"]:
                data.setdefault(bench, {})[pol] = stats
    return data


def plot_normalized(data, benches, metric, ylabel, title, outfile, baseline="RR"):
    fig, ax = plt.subplots(figsize=(max(8, len(benches) * 1.2), 4.2))
    width = 0.2
    x = np.arange(len(benches))
    for i, pol in enumerate(POLICIES):
        vals = []
        for b in benches:
            base = data.get(b, {}).get(baseline)
            cur = data.get(b, {}).get(pol)
            if not base or not cur or not base.get(metric):
                vals.append(0.0)
            else:
                vals.append(cur[metric] / base[metric])
        offset = (i - (len(POLICIES) - 1) / 2) * width
        bars = ax.bar(x + offset, vals, width, label=pol, color=COLORS[pol],
                      edgecolor="black", linewidth=0.4)
        # annotate values above bars
        for bar, v in zip(bars, vals):
            if v > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, v + 0.02,
                        f"{v:.2f}", ha="center", va="bottom", fontsize=7)

    ax.axhline(1.0, color="black", linewidth=0.6, linestyle="--", alpha=0.6)
    ax.set_xticks(x)
    ax.set_xticklabels(benches, rotation=15, ha="right")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.legend(loc="best", fontsize=9, ncol=4)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(outfile, dpi=150)
    print(f"wrote {outfile}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("run_dir", nargs="?", default=None)
    args = p.parse_args()

    root = Path(__file__).resolve().parent.parent / "runs"
    if args.run_dir:
        run_dir = Path(args.run_dir).resolve()
    else:
        runs = sorted(root.glob("run*"),
                      key=lambda p: int(p.name[3:]) if p.name[3:].isdigit() else 0)
        if not runs:
            print("no runN/ dirs under worklogs/runs/", file=sys.stderr)
            sys.exit(1)
        run_dir = runs[-1]
    print(f"using {run_dir}")

    rr_dir = run_dir / "RR"
    if not rr_dir.exists():
        print(f"no RR/ baseline in {run_dir}", file=sys.stderr)
        sys.exit(1)
    benches = [log.name.replace(".perf2.log", "")
               for log in sorted(rr_dir.glob("*.perf2.log"))]
    if not benches:
        print("no .perf2.log files found in RR/", file=sys.stderr)
        sys.exit(1)
    print(f"benches: {benches}")

    data = collect(run_dir, benches)
    for b in benches:
        for pol, s in data.get(b, {}).items():
            if s["rmiss"] and s["instrs"]:
                s["mpki"] = s["rmiss"] * 1000.0 / s["instrs"]
            else:
                s["mpki"] = 0
    benches = [b for b in benches if "RR" in data.get(b, {})]

    plot_normalized(
        data, benches, "ipc",
        "IPC normalized to RR",
        f"IPC speedup over RR ({run_dir.name})",
        run_dir / "ipc_vs_rr.png",
    )
    plot_normalized(
        data, benches, "mpki",
        "MPKI normalized to RR",
        f"L1D read-MPKI ratio over RR ({run_dir.name})",
        run_dir / "mpki_vs_rr.png",
    )


if __name__ == "__main__":
    main()
