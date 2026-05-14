#!/usr/bin/env python3
"""
CPL log image exporter — saves one PNG per snapshot.

Usage:
  python3 plot_cpl.py <logfile> [--out <dir>]

Example:
  python3 plot_cpl.py ../runs/one_gCAWS_20260514_112723/gCAWS/kmeans.log
  python3 plot_cpl.py kmeans.log --out /tmp/cpl_frames
"""

import sys
import re
import os
import argparse
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ── parsing ───────────────────────────────────────────────────────────────────

def parse_log(path):
    pattern_line = re.compile(r"CPL_LOG:\s+(?:kernel=(\d+)\s+)?cycle=(\d+)(?:\s+core=(\d+))?(.*)")
    pattern_warp = re.compile(r"w(\d+)=(\d+),(\d+),(\d+)(?:,(\d+))?")

    snapshots = []
    with open(path) as f:
        for line in f:
            m = pattern_line.search(line)
            if not m:
                continue
            kernel = int(m.group(1)) if m.group(1) is not None else 0
            cycle  = int(m.group(2))
            core   = int(m.group(3)) if m.group(3) is not None else 0
            wids, instr, stall, crit, active = [], [], [], [], []
            for wm in pattern_warp.finditer(m.group(4)):
                wids.append(int(wm.group(1)))
                instr.append(int(wm.group(2)))
                stall.append(int(wm.group(3)))
                crit.append(int(wm.group(4)))
                active.append(int(wm.group(5)) if wm.group(5) is not None else 1)
            if wids:
                snapshots.append({"kernel": kernel, "cycle": cycle, "core": core,
                                   "wids": wids, "instr": instr,
                                   "stall": stall, "crit": crit, "active": active})

    if not snapshots:
        print(f"ERROR: no CPL_LOG lines found in {path}", file=sys.stderr)
        sys.exit(1)

    num_warps = max(max(s["wids"]) for s in snapshots) + 1
    return snapshots, num_warps


def to_arrays(snap, num_warps):
    instr  = np.zeros(num_warps)
    stall  = np.zeros(num_warps)
    crit   = np.zeros(num_warps)
    is_act = np.zeros(num_warps, dtype=bool)
    for i, wid in enumerate(snap["wids"]):
        instr[wid]  = snap["instr"][i]
        stall[wid]  = snap["stall"][i]
        crit[wid]   = snap["crit"][i]
        is_act[wid] = bool(snap["active"][i])
    return instr, stall, crit, is_act


# ── per-snapshot figure ───────────────────────────────────────────────────────

def save_snapshot(snap, num_warps, cmap, out_path, log_name):
    instr_v, stall_v, crit_v, is_act = to_arrays(snap, num_warps)
    x = np.arange(num_warps)

    fig = plt.figure(figsize=(14, 7))
    fig.suptitle(
        f"gCAWS CPL  |  {log_name}  |  kernel={snap['kernel']}  cycle={snap['cycle']:,}  core={snap['core']}",
        fontsize=12
    )

    ax_crit  = fig.add_axes([0.06, 0.54, 0.88, 0.36])
    ax_instr = fig.add_axes([0.06, 0.10, 0.40, 0.32])
    ax_stall = fig.add_axes([0.54, 0.10, 0.40, 0.32])

    # criticality bars: active=colormap, inactive=gray
    c_max = crit_v[is_act].max() if is_act.any() else 1.0
    colors = np.where(is_act[:, None],
                      cmap(crit_v / (c_max if c_max > 0 else 1.0)),
                      np.array([[0.75, 0.75, 0.75, 1.0]]))
    ax_crit.bar(x, crit_v, width=0.7, color=colors)

    # y-axis: zoom to the local value range to show warp-to-warp differences clearly
    def ylim(vals):
        active = vals[vals > 0]
        if len(active) == 0:
            return 0, 1
        lo = active.min()
        hi = active.max()
        margin = (hi - lo) * 0.3 if hi > lo else hi * 0.1
        return max(0, lo - margin), hi + margin

    ax_crit.set_ylim(*ylim(crit_v))
    ax_instr.set_ylim(*ylim(instr_v))
    ax_stall.set_ylim(*ylim(stall_v))

    instr_colors = np.where(is_act, "#5b9bd5", "#bbbbbb")
    stall_colors = np.where(is_act, "#ed7d31", "#bbbbbb")
    ax_instr.bar(x, instr_v, width=0.7, color=instr_colors)
    ax_stall.bar(x, stall_v, width=0.7, color=stall_colors)

    # most-critical warp marker
    best_wid = int(np.argmax(crit_v))
    ax_crit.axvline(x=best_wid, color="red", linewidth=1.5, linestyle="--", alpha=0.8)

    active_count = int(is_act.sum())
    ax_crit.text(0.99, 0.97,
                 f"kernel={snap['kernel']}  active warps: {active_count}\n"
                 f"most critical: w{best_wid}  crit={int(crit_v[best_wid]):,}",
                 transform=ax_crit.transAxes, ha="right", va="top", fontsize=9,
                 bbox=dict(boxstyle="round,pad=0.3", fc="white", alpha=0.85))

    for ax, title, color in [
        (ax_crit,  "Criticality  (nInst x CPI + nStall)", "#c00"),
        (ax_instr, "instr_count  (cumulative issued)",     "#5b9bd5"),
        (ax_stall, "stall_cycles (cumulative stall)",      "#c07020"),
    ]:
        ax.set_title(title, fontsize=9, color=color, pad=3)
        ax.set_xlabel("warp id", fontsize=8)
        ax.set_xticks(x[::2])
        ax.tick_params(labelsize=8)
        ax.grid(axis="y", linestyle=":", alpha=0.45)

    fig.savefig(out_path, dpi=120, bbox_inches="tight")
    plt.close(fig)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    _script_dir = os.path.dirname(os.path.abspath(__file__))
    _default_log = os.path.join(
        _script_dir, "../runs/one_gCAWS_20260514_112723/gCAWS/kmeans.log"
    )

    ap = argparse.ArgumentParser(description="Save CPL log snapshots as PNG images")
    ap.add_argument("logfile", nargs="?", default=_default_log,
                    help="simx log file containing CPL_LOG lines")
    ap.add_argument("--out", default=None,
                    help="output directory (default: <logfile_dir>/cpl_frames/)")
    args = ap.parse_args()

    log_path = os.path.abspath(args.logfile)
    out_dir  = args.out or os.path.join(os.path.dirname(log_path), "cpl_frames")
    os.makedirs(out_dir, exist_ok=True)

    print(f"Parsing {log_path} ...")
    snapshots, num_warps = parse_log(log_path)
    print(f"  {len(snapshots)} snapshots  |  {num_warps} warps")

    cmap     = plt.get_cmap("RdYlGn_r")
    log_name = os.path.basename(log_path)
    total    = len(snapshots)

    print(f"Saving frames to {out_dir}/")
    for i, snap in enumerate(snapshots):
        fname = f"cpl_k{snap['kernel']:03d}_c{snap['cycle']:08d}_core{snap['core']:02d}.png"
        save_snapshot(snap, num_warps, cmap,
                      os.path.join(out_dir, fname), log_name)
        if (i + 1) % 50 == 0 or (i + 1) == total:
            print(f"  {i+1}/{total}")

    print("Done.")


if __name__ == "__main__":
    main()
