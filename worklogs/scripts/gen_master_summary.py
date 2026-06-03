#!/usr/bin/env python3
"""Aggregate worklogs/runs/auto20_* summary.tsv files into a single overview.

Outputs:
  worklogs/runs/master_sweep_summary.csv  — raw flat table
  worklogs/runs/master_sweep_summary.md   — human-readable, top sweet-spots
"""
import csv
import os
import re
import sys
from collections import defaultdict

LOG_BASE = "/data/URP_26_spring/bbosseong/vortex_aware_scheduler/worklogs/runs"

# auto20_kmeans_p256_WG1_M2
TAG_RE = re.compile(
    r"^auto20_(?P<workload>kmeans|bfs|sgemm3)_"
    r"(?P<input>[^_]+(?:_heavyhub)?)_"
    r"WG(?P<wg>1|small|mid|large)_"
    r"M(?P<mshr>\d+)$"
)

def parse_tag(name):
    m = TAG_RE.match(name)
    if not m:
        return None
    return m.groupdict()

def to_float(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None

def collect():
    rows = []
    for d in sorted(os.listdir(LOG_BASE)):
        if not d.startswith("auto20_"):
            continue
        parsed = parse_tag(d)
        if not parsed:
            continue
        tsv = os.path.join(LOG_BASE, d, "summary.tsv")
        if not os.path.exists(tsv):
            continue
        with open(tsv) as f:
            reader = csv.DictReader(f, delimiter="\t")
            for r in reader:
                row = {
                    "workload": parsed["workload"],
                    "input": parsed["input"],
                    "wg": parsed["wg"],
                    "mshr": int(parsed["mshr"]),
                    "policy": r.get("policy"),
                    "rc": r.get("rc"),
                    "IPC": to_float(r.get("IPC")),
                    "cycles": to_float(r.get("cycles")),
                    "L1_MPKI": to_float(r.get("L1_MPKI")),
                    "l1_hit": to_float(r.get("l1_hit")),
                    "mlat": to_float(r.get("mlat")),
                    "dc_mshr": to_float(r.get("dc_mshr")),
                    "n_kend": to_float(r.get("n_kend")),
                    "tag": d,
                }
                rows.append(row)
    return rows

def write_csv(rows, path):
    if not rows:
        return
    keys = ["workload", "input", "wg", "mshr", "policy", "rc",
            "IPC", "cycles", "L1_MPKI", "l1_hit", "mlat", "dc_mshr", "n_kend", "tag"]
    with open(path, "w") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)

def compute_deltas(rows):
    """For each (workload, input, wg, mshr) group, compute Δ vs RR for IPC and MPKI."""
    cells = defaultdict(dict)
    for r in rows:
        key = (r["workload"], r["input"], r["wg"], r["mshr"])
        cells[key][r["policy"]] = r
    out = []
    for key, by_pol in cells.items():
        rr = by_pol.get("RR")
        gto = by_pol.get("GTO")
        gcaws = by_pol.get("gCAWS")
        if not (rr and gto and gcaws):
            continue
        if rr["IPC"] is None or gto["IPC"] is None or gcaws["IPC"] is None:
            continue
        wl, inp, wg, mshr = key
        def delta(p, base):
            if p is None or base is None or base == 0:
                return None
            return (p - base) / base * 100.0
        out.append({
            "workload": wl,
            "input": inp,
            "wg": wg,
            "mshr": mshr,
            "RR_IPC": rr["IPC"],
            "GTO_IPC": gto["IPC"],
            "gCAWS_IPC": gcaws["IPC"],
            "GTO_dIPC": delta(gto["IPC"], rr["IPC"]),
            "gCAWS_dIPC": delta(gcaws["IPC"], rr["IPC"]),
            "RR_MPKI": rr["L1_MPKI"],
            "GTO_MPKI": gto["L1_MPKI"],
            "gCAWS_MPKI": gcaws["L1_MPKI"],
            "GTO_dMPKI": delta(gto["L1_MPKI"], rr["L1_MPKI"]),
            "gCAWS_dMPKI": delta(gcaws["L1_MPKI"], rr["L1_MPKI"]),
            "max_dIPC": max(abs(delta(gto["IPC"], rr["IPC"]) or 0),
                            abs(delta(gcaws["IPC"], rr["IPC"]) or 0)),
        })
    return out

def fmt_pct(v):
    if v is None:
        return "-"
    sign = "+" if v >= 0 else ""
    return f"{sign}{v:.1f}%"

def fmt_ipc(v):
    if v is None:
        return "-"
    return f"{v:.3f}"

def write_md(rows, deltas, path):
    with open(path, "w") as f:
        f.write("# Master sweep — scheduler differentiation summary\n\n")
        f.write(f"Total rows: **{len(rows)}**  (cells with all 3 policies: **{len(deltas)}**)\n\n")
        f.write("## Top 20 sweet spots (by max |Δ IPC vs RR|)\n\n")
        f.write("| workload | input | wg | mshr | RR IPC | GTO Δ | gCAWS Δ | RR MPKI | GTO MPKI Δ | gCAWS MPKI Δ |\n")
        f.write("|---|---|---|---|---|---|---|---|---|---|\n")
        for d in sorted(deltas, key=lambda x: -(x["max_dIPC"] or 0))[:20]:
            f.write(
                f"| {d['workload']} | {d['input']} | {d['wg']} | {d['mshr']} "
                f"| {fmt_ipc(d['RR_IPC'])} | {fmt_pct(d['GTO_dIPC'])} | {fmt_pct(d['gCAWS_dIPC'])} "
                f"| {fmt_ipc(d['RR_MPKI'])} | {fmt_pct(d['GTO_dMPKI'])} | {fmt_pct(d['gCAWS_dMPKI'])} |\n"
            )

        for wl in ("kmeans", "bfs", "sgemm3"):
            wl_cells = [d for d in deltas if d["workload"] == wl]
            if not wl_cells:
                continue
            f.write(f"\n## {wl} — top 10 cells\n\n")
            f.write("| input | wg | mshr | RR IPC | GTO Δ | gCAWS Δ | RR MPKI | GTO MPKI Δ | gCAWS MPKI Δ |\n")
            f.write("|---|---|---|---|---|---|---|---|---|\n")
            for d in sorted(wl_cells, key=lambda x: -(x["max_dIPC"] or 0))[:10]:
                f.write(
                    f"| {d['input']} | {d['wg']} | {d['mshr']} "
                    f"| {fmt_ipc(d['RR_IPC'])} | {fmt_pct(d['GTO_dIPC'])} | {fmt_pct(d['gCAWS_dIPC'])} "
                    f"| {fmt_ipc(d['RR_MPKI'])} | {fmt_pct(d['GTO_dMPKI'])} | {fmt_pct(d['gCAWS_dMPKI'])} |\n"
                )

def main():
    rows = collect()
    csv_path = os.path.join(LOG_BASE, "master_sweep_summary.csv")
    md_path  = os.path.join(LOG_BASE, "master_sweep_summary.md")
    write_csv(rows, csv_path)
    deltas = compute_deltas(rows)
    write_md(rows, deltas, md_path)
    print(f"wrote {csv_path}", file=sys.stderr)
    print(f"wrote {md_path}", file=sys.stderr)
    print(f"  rows={len(rows)}, full-cells={len(deltas)}", file=sys.stderr)

if __name__ == "__main__":
    main()
