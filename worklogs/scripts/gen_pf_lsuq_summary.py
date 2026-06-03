#!/usr/bin/env python3
"""Aggregate auto30_pf_lsuq{8,16}_summary.tsv into comparison tables.

Outputs to worklogs/runs/auto30_pf_lsuq_summary.md:
  * IPC heatmap per LSUQ × policy (p rows, f cols)
  * scrb % heatmap
  * Per-(p,f) GTO Δ vs RR, gCAWS Δ vs RR
  * Top sweet spots (largest scheduler differentiation)
"""
import csv
import os
import sys
from collections import defaultdict

BASE = "/data/URP_26_spring/bbosseong/vortex_aware_scheduler/worklogs/runs"
LSUQS = [8, 16]
POLICIES = ["RR", "GTO", "gCAWS"]

def load(lsuq):
    f = os.path.join(BASE, f"auto30_pf_lsuq{lsuq}_summary.tsv")
    if not os.path.exists(f):
        return None
    rows = []
    with open(f) as fh:
        rd = csv.DictReader(fh, delimiter="\t")
        for r in rd:
            rows.append(r)
    return rows

def to_f(v):
    try: return float(v)
    except (ValueError, TypeError): return None
def to_pct(v):
    if not v: return None
    return to_f(v.rstrip("%"))

def index(rows):
    # (lsuq, p, f, policy) -> rec
    out = {}
    for r in rows:
        try:
            p = int(r["p"]); f = int(r["f"])
        except (ValueError, KeyError): continue
        out[(p, f, r["policy"])] = r
    return out

def fmt(v, w=6, p=2):
    if v is None: return "  -  ".center(w)
    if isinstance(v, float): return f"{v:.{p}f}".rjust(w)
    return str(v).rjust(w)

def write_md(lsuq8, lsuq16, out):
    with open(out, "w") as f:
        f.write(f"# p×f sweep @ LSUQ ∈ {{8, 16}} — kmeans summary\n\n")
        f.write("Config: NUM_WARPS=32 NUM_THREADS=16 L1=16KB MSHR=16 WG=128 IW=1\n\n")
        for lsuq, data in [(8, lsuq8), (16, lsuq16)]:
            if not data:
                f.write(f"## LSUQ={lsuq}: (missing data)\n\n")
                continue
            data_idx = index(data)
            ps = sorted({k[0] for k in data_idx})
            fs = sorted({k[1] for k in data_idx})

            for metric, mname in [("IPC", "IPC"), ("scrb_pct", "scrb %"), ("issued_pct", "issued %"), ("L1_MPKI", "L1 MPKI")]:
                f.write(f"## LSUQ={lsuq} — {mname} (rows=p, cols=f) × {POLICIES}\n\n")
                for pol in POLICIES:
                    f.write(f"### {pol}\n\n")
                    head = "| p ↓ / f → | " + " | ".join(str(x) for x in fs) + " |\n"
                    f.write(head)
                    f.write("|" + "---|" * (len(fs)+1) + "\n")
                    for p in ps:
                        line = [f"**{p}**"]
                        for ff in fs:
                            rec = data_idx.get((p, ff, pol))
                            if not rec:
                                line.append("-")
                            else:
                                v = rec.get(metric, "?")
                                if metric.endswith("_pct"):
                                    line.append(v if v != "?" else "-")
                                else:
                                    fv = to_f(v)
                                    line.append(f"{fv:.2f}" if fv is not None else "-")
                        f.write("| " + " | ".join(line) + " |\n")
                    f.write("\n")

            # Per-(p, f) scheduler comparison
            f.write(f"## LSUQ={lsuq} — Scheduler Δ vs RR (IPC %)\n\n")
            f.write("| p | f | RR IPC | GTO Δ | gCAWS Δ |\n|---|---|---|---|---|\n")
            for p in ps:
                for ff in fs:
                    rr = data_idx.get((p, ff, "RR"))
                    gt = data_idx.get((p, ff, "GTO"))
                    gc = data_idx.get((p, ff, "gCAWS"))
                    if not (rr and gt and gc): continue
                    rr_i = to_f(rr.get("IPC")); gt_i = to_f(gt.get("IPC")); gc_i = to_f(gc.get("IPC"))
                    if rr_i is None or rr_i == 0: continue
                    if gt_i is None or gc_i is None: continue
                    g_d = (gt_i - rr_i) / rr_i * 100
                    c_d = (gc_i - rr_i) / rr_i * 100
                    f.write(f"| {p} | {ff} | {rr_i:.2f} | {g_d:+.2f}% | {c_d:+.2f}% |\n")
            f.write("\n")

        # Cross-LSUQ comparison: LSUQ=16 IPC / LSUQ=8 IPC
        if lsuq8 and lsuq16:
            l8 = index(lsuq8); l16 = index(lsuq16)
            f.write("## LSUQ=16 vs LSUQ=8 IPC Δ (per (p,f,policy))\n\n")
            f.write("| p | f | policy | IPC@8 | IPC@16 | Δ |\n|---|---|---|---|---|---|\n")
            ps_common = sorted({k[0] for k in l8} & {k[0] for k in l16})
            fs_common = sorted({k[1] for k in l8} & {k[1] for k in l16})
            for p in ps_common:
                for ff in fs_common:
                    for pol in POLICIES:
                        r8 = l8.get((p, ff, pol)); r16 = l16.get((p, ff, pol))
                        if not (r8 and r16): continue
                        i8 = to_f(r8.get("IPC")); i16 = to_f(r16.get("IPC"))
                        if i8 is None or i16 is None or i8 == 0: continue
                        d = (i16 - i8) / i8 * 100
                        f.write(f"| {p} | {ff} | {pol} | {i8:.2f} | {i16:.2f} | {d:+.2f}% |\n")
            f.write("\n")

def main():
    l8 = load(8)
    l16 = load(16)
    out = os.path.join(BASE, "auto30_pf_lsuq_summary.md")
    write_md(l8, l16, out)
    print(f"wrote {out}")

if __name__ == "__main__":
    main()
