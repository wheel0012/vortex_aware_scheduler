#!/usr/bin/env python3
"""Compare warp-scheduler decisions across policies from WarpSchedTrace CSVs.

Single-PNG output with four diagnostics:
  1. Issue timeline scatter (warp on Y, cycle on X) per policy
  2. Per-warp grant-share bar chart (which warps each policy favors)
  3. Per-policy grant-distribution entropy over rolling cycle windows
     (lower entropy = more concentrated on few warps = more "scheduler effect")
  4. Cumulative "differs-from-baseline" by issue event index
     (per non-baseline policy, count of issues where selected wid differs
      from baseline's selection at the same Nth issue event)

Usage:
  compare_policies.py \
    --csv RR:run4/RR/kmeans.csv \
    --csv GTO:run4/GTO/kmeans.csv \
    --csv gCAWS:run4/gCAWS/kmeans.csv \
    --out compare_kmeans.png \
    [--cycle-from N] [--cycle-to N] [--window 10000]

The first --csv argument is treated as the baseline for the divergence panel.
"""
import argparse
import csv
import math
import os
import sys
import tempfile
from collections import Counter, defaultdict, deque
from pathlib import Path

# Reuse helpers from the existing analyze script if importable.
SCRIPT_DIR = Path(__file__).resolve().parent
SIMX_DIR = SCRIPT_DIR.parent.parent / "sim" / "simx"
if SIMX_DIR.is_dir():
    sys.path.insert(0, str(SIMX_DIR))
try:
    from analyze_warp_sched_trace import (
        parse_int, parse_float, parse_bool, parse_score_vector,
    )
except ImportError:
    def parse_int(v):
        v = (v or "").strip()
        try:
            return int(v, 0) if v else None
        except ValueError:
            return None

    def parse_float(v):
        v = (v or "").strip()
        try:
            return float(v) if v else None
        except ValueError:
            return None

    def parse_bool(v):
        return str(v).strip().lower() in {"1", "true", "yes", "y"}

    def parse_score_vector(v):
        if not v:
            return []
        out = []
        for tok in v.split("|"):
            try:
                out.append(float(tok))
            except ValueError:
                out.append(None)
        return out


def import_matplotlib():
    os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "matplotlib"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    return plt


def parse_csv_arg(value):
    if ":" not in value:
        raise argparse.ArgumentTypeError(
            f"--csv expects LABEL:PATH, got {value!r}")
    label, _, path = value.partition(":")
    label = label.strip()
    path = Path(path.strip())
    if not label or not path.is_file():
        raise argparse.ArgumentTypeError(
            f"--csv {value!r}: empty label or missing file")
    return label, path


def stream_issued(path, cycle_from=None, cycle_to=None):
    """Yield (cycle, slot, wid, score_vec) for rows where issued=true."""
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            if not parse_bool(row.get("issued", "")):
                continue
            cycle = parse_int(row.get("cycle"))
            if cycle is None:
                continue
            if cycle_from is not None and cycle < cycle_from:
                continue
            if cycle_to is not None and cycle > cycle_to:
                break
            wid = parse_int(row.get("actual_wid"))
            if wid is None:
                wid = parse_int(row.get("selected_wid"))
            if wid is None or wid < 0:
                continue
            slot = parse_int(row.get("issue_slot")) or 0
            score_vec = parse_score_vector(row.get("score_vector", ""))
            yield cycle, slot, wid, score_vec


def collect(path, cycle_from, cycle_to):
    cycles, wids, slots, score_at_select = [], [], [], []
    grants = Counter()
    score_matrix = []  # list of (cycle, [score per wid])
    nth_issue_wids = []  # for divergence alignment
    for cycle, slot, wid, vec in stream_issued(path, cycle_from, cycle_to):
        cycles.append(cycle)
        wids.append(wid)
        slots.append(slot)
        grants[wid] += 1
        nth_issue_wids.append(wid)
        if vec:
            score_matrix.append((cycle, vec))
            sc = vec[wid] if 0 <= wid < len(vec) else None
            score_at_select.append(sc)
        else:
            score_at_select.append(None)
    return {
        "cycles": cycles,
        "wids": wids,
        "slots": slots,
        "grants": grants,
        "score_matrix": score_matrix,
        "nth_issue_wids": nth_issue_wids,
        "score_at_select": score_at_select,
    }


def rolling_entropy(wids, cycles, window):
    """Per-window Shannon entropy of grant distribution (base 2)."""
    if not wids or window <= 0:
        return [], []
    # Group by cycle window.
    bins = defaultdict(Counter)
    for c, w in zip(cycles, wids):
        bins[c // window][w] += 1
    xs, ys = [], []
    for bid in sorted(bins):
        counter = bins[bid]
        n = sum(counter.values())
        if n == 0:
            continue
        H = 0.0
        for v in counter.values():
            p = v / n
            H -= p * math.log2(p)
        xs.append(bid * window)
        ys.append(H)
    return xs, ys


def cumulative_diff(baseline_wids, other_wids):
    """At each Nth issue event, cumulative count of (other != baseline)."""
    n = min(len(baseline_wids), len(other_wids))
    cum, out = 0, []
    for i in range(n):
        if baseline_wids[i] != other_wids[i]:
            cum += 1
        out.append(cum)
    return out


def downsample(xs, ys, max_points=2000):
    if len(xs) <= max_points:
        return xs, ys
    stride = max(1, len(xs) // max_points)
    return xs[::stride], ys[::stride]


def build_heatmap(score_matrix, num_warps, num_bins=200):
    """Returns 2D array [num_warps][num_bins] = mean score over bin."""
    if not score_matrix:
        return None, None, None
    cmin = score_matrix[0][0]
    cmax = score_matrix[-1][0]
    if cmax == cmin:
        cmax = cmin + 1
    bin_width = max(1, (cmax - cmin) // num_bins)
    actual_bins = (cmax - cmin) // bin_width + 1
    acc = [[0.0] * actual_bins for _ in range(num_warps)]
    cnt = [[0] * actual_bins for _ in range(num_warps)]
    for cycle, vec in score_matrix:
        b = (cycle - cmin) // bin_width
        if not (0 <= b < actual_bins):
            continue
        for w, sc in enumerate(vec):
            if w >= num_warps or sc is None or (isinstance(sc, float) and math.isnan(sc)):
                continue
            acc[w][b] += sc
            cnt[w][b] += 1
    mat = [[acc[w][b] / cnt[w][b] if cnt[w][b] else 0.0
            for b in range(actual_bins)] for w in range(num_warps)]
    return mat, cmin, bin_width


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", action="append", required=True, type=parse_csv_arg,
                    help="LABEL:PATH (repeatable); first one is the baseline")
    ap.add_argument("--cycle-from", type=int, default=None)
    ap.add_argument("--cycle-to", type=int, default=None)
    ap.add_argument("--window", type=int, default=20000,
                    help="cycles per entropy bucket (default 20000)")
    ap.add_argument("--zoom-cycles", type=int, default=3000,
                    help="length of issue-timeline zoom window (default 3000)")
    ap.add_argument("--zoom-from", type=int, default=None,
                    help="start cycle of zoom (default: 25%% into the trace)")
    ap.add_argument("--num-warps", type=int, default=32)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    plt = import_matplotlib()
    policies = args.csv  # list of (label, path)
    print(f"loading {len(policies)} CSV(s)...", file=sys.stderr)
    data = {label: collect(path, args.cycle_from, args.cycle_to)
            for label, path in policies}

    for label, d in data.items():
        n = len(d["wids"])
        print(f"  {label}: {n} issued events, {len(d['grants'])} distinct wids",
              file=sys.stderr)

    baseline_label = policies[0][0]
    baseline_wids = data[baseline_label]["nth_issue_wids"]

    # Pick a zoom window from the baseline trace.
    bcycles = data[baseline_label]["cycles"]
    if bcycles:
        if args.zoom_from is None:
            zf = bcycles[len(bcycles) // 4]
        else:
            zf = args.zoom_from
        zt = zf + args.zoom_cycles
    else:
        zf, zt = 0, args.zoom_cycles

    n_pol = len(policies)
    fig = plt.figure(figsize=(5 * n_pol, 14))
    gs = fig.add_gridspec(4, n_pol,
                          height_ratios=[2.4, 2.4, 2.0, 2.0],
                          hspace=0.45, wspace=0.25)

    # Row 1: zoom-in issue timeline.
    for i, (label, _) in enumerate(policies):
        ax = fig.add_subplot(gs[0, i])
        d = data[label]
        xs, ys = [], []
        for c, w in zip(d["cycles"], d["wids"]):
            if zf <= c < zt:
                xs.append(c)
                ys.append(w)
        ax.scatter(xs, ys, s=4, alpha=0.7)
        ax.set_xlim(zf, zt)
        ax.set_ylim(-0.5, args.num_warps - 0.5)
        ax.set_title(f"{label}: issue timeline [{zf}..{zt}]")
        ax.set_xlabel("cycle")
        if i == 0:
            ax.set_ylabel("wid (issued)")
        ax.grid(True, alpha=0.25)

    # Row 2: criticality heatmap (full duration).
    vmin_g, vmax_g = None, None
    mats = {}
    for label, _ in policies:
        mat, cmin, bw = build_heatmap(data[label]["score_matrix"], args.num_warps)
        mats[label] = (mat, cmin, bw)
        if mat is None:
            continue
        flat = [v for row in mat for v in row if v > 0]
        if flat:
            lo, hi = min(flat), max(flat)
            vmin_g = lo if vmin_g is None else min(vmin_g, lo)
            vmax_g = hi if vmax_g is None else max(vmax_g, hi)
    for i, (label, _) in enumerate(policies):
        ax = fig.add_subplot(gs[1, i])
        mat, cmin, bw = mats[label]
        if mat is None:
            ax.text(0.5, 0.5, "no score data", ha="center", va="center",
                    transform=ax.transAxes)
            ax.set_title(f"{label}: criticality heatmap")
            continue
        nbins = len(mat[0])
        extent = [cmin, cmin + nbins * bw, args.num_warps - 0.5, -0.5]
        im = ax.imshow(mat, aspect="auto", extent=extent, origin="upper",
                       vmin=vmin_g, vmax=vmax_g, cmap="viridis",
                       interpolation="nearest")
        ax.set_title(f"{label}: criticality (warp × cycle)")
        ax.set_xlabel("cycle")
        if i == 0:
            ax.set_ylabel("wid")
        fig.colorbar(im, ax=ax, fraction=0.04, pad=0.02)

    # Row 3: grant share + entropy timeline.
    ax_share = fig.add_subplot(gs[2, 0:max(1, n_pol // 2)])
    width = 0.8 / n_pol
    wids_axis = list(range(args.num_warps))
    for i, (label, _) in enumerate(policies):
        grants = data[label]["grants"]
        total = sum(grants.values()) or 1
        ys = [100.0 * grants.get(w, 0) / total for w in wids_axis]
        xs = [w + (i - (n_pol - 1) / 2) * width for w in wids_axis]
        ax_share.bar(xs, ys, width=width, label=label, alpha=0.85)
    ax_share.set_xlabel("wid")
    ax_share.set_ylabel("% of total grants")
    ax_share.set_title("Per-warp grant share")
    ax_share.legend(loc="best", fontsize="small")
    ax_share.grid(True, alpha=0.25, axis="y")

    ax_entropy = fig.add_subplot(gs[2, max(1, n_pol // 2):n_pol])
    for label, _ in policies:
        xs, ys = rolling_entropy(data[label]["wids"], data[label]["cycles"],
                                 args.window)
        xs, ys = downsample(xs, ys)
        ax_entropy.plot(xs, ys, linewidth=1.0, alpha=0.8, label=label)
    ax_entropy.set_xlabel("cycle")
    ax_entropy.set_ylabel(f"H(grant dist) [bits], window={args.window}c")
    ax_entropy.set_title("Grant entropy over time  (lower = more concentrated)")
    ax_entropy.legend(loc="best", fontsize="small")
    ax_entropy.grid(True, alpha=0.25)
    ax_entropy.axhline(math.log2(args.num_warps), linestyle="--",
                       linewidth=0.7, alpha=0.4,
                       label="uniform max")

    # Row 4: cumulative divergence vs baseline.
    ax_div = fig.add_subplot(gs[3, :])
    for label, _ in policies:
        if label == baseline_label:
            continue
        diffs = cumulative_diff(baseline_wids, data[label]["nth_issue_wids"])
        xs = list(range(len(diffs)))
        xs, diffs = downsample(xs, diffs)
        ax_div.plot(xs, diffs, linewidth=1.2, label=f"{label} vs {baseline_label}")
    ax_div.set_xlabel("Nth issue event")
    ax_div.set_ylabel(f"cumulative #(wid != {baseline_label})")
    ax_div.set_title(
        "Cumulative grant divergence vs baseline  "
        "(flat ⇒ same decisions ⇒ policy = baseline)")
    ax_div.legend(loc="best", fontsize="small")
    ax_div.grid(True, alpha=0.25)

    fig.suptitle("Warp-scheduler policy comparison", fontsize=14, y=0.995)
    fig.tight_layout(rect=[0, 0, 1, 0.99])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=140)
    plt.close(fig)
    print(f"wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
