#!/usr/bin/env python3
"""
BFS imbalanced (hub/scale-free) graph generator.

Creates graphs whose degree distribution follows a power law: most nodes are
low-degree, while a small fraction of "hub" nodes have very high degree.
Visiting a hub during BFS explodes the next frontier (the hub warp pushes
hundreds of edges) while non-hub warps idle, naturally producing warp
imbalance — the scenario that CAWA gCAWS is designed to accelerate.

Output format (Rodinia bfs):
  N                               # number of nodes
  start_0 ne_0                    # adjacency list start offsets + degrees
  start_1 ne_1
  ...
  source                          # BFS source node (always 0 here)
  E                               # total edges
  dest_0 cost_0                   # edge destinations + costs (cost 1-10)
  ...

Usage:
  ./gen_hub_graph.py SIZE PROFILE OUTPUT
  ./gen_hub_graph.py 16384 heavy graph16k_heavyhub.txt
  ./gen_hub_graph.py 131072 extreme /tmp/graph128k_extreme.txt

Profiles (degree distribution):
  mild     : 50% deg 1-3,  40% 4-10,  9% 11-30,  1% 31-100        (light tail)
  medium   : 70% deg 1-3,  25% 4-15,  4.5% 20-80,  0.5% 100-500   (default 'hub')
  heavy    : 85% deg 1-2,  12% 3-10,  2.5% 20-100, 0.5% 200-1000  (default 'heavyhub')
  extreme  : 92% deg 1-2,  6% 3-8,    1.8% 20-100, 0.2% 500-3000  (paper-style critical warp)

Source node 0 is forced to degree >= 200 so the first BFS step always expands
a sizable frontier (otherwise the search may die after one level for small
random sources).

Random seed is fixed at 42 for full reproducibility.
"""
import random
import sys

PROFILES = {
    'mild':    [(0.50, 1, 3),  (0.40, 4, 10),  (0.09,  11,  30),  (0.01, 31, 100)],
    'medium':  [(0.70, 1, 3),  (0.25, 4, 15),  (0.045, 20,  80),  (0.005, 100, 500)],
    'heavy':   [(0.85, 1, 2),  (0.12, 3, 10),  (0.025, 20, 100),  (0.005, 200, 1000)],
    'extreme': [(0.92, 1, 2),  (0.06, 3,  8),  (0.018, 20, 100),  (0.002, 500, 3000)],
}

def sample_degree(dist):
    r = random.random()
    cum = 0
    for prob, lo, hi in dist:
        cum += prob
        if r < cum:
            return random.randint(lo, hi)
    return random.randint(1, 3)

def random_dests(src, k, n):
    """k unique destinations in [0,n), excluding src."""
    out = set()
    while len(out) < k:
        cand = random.randint(0, n - 1)
        if cand != src:
            out.add(cand)
    return out

def generate(n, profile, out_path, seed=42, source_min_deg=200):
    if profile not in PROFILES:
        raise ValueError(f"unknown profile {profile!r}, choose from {list(PROFILES)}")
    random.seed(seed)
    dist = PROFILES[profile]

    degrees = [sample_degree(dist) for _ in range(n)]
    degrees[0] = max(degrees[0], source_min_deg)  # ensure source expands

    starts = [0]
    for d in degrees[:-1]:
        starts.append(starts[-1] + d)
    total_edges = sum(degrees)

    with open(out_path, 'w') as f:
        f.write(f"{n}\n")
        for s, d in zip(starts, degrees):
            f.write(f"{s} {d}\n")
        f.write("0\n")
        f.write(f"{total_edges}\n")
        for src, d in enumerate(degrees):
            for dst in random_dests(src, d, n):
                cost = random.randint(1, 10)
                f.write(f"{dst} {cost}\n")

    max_deg = max(degrees)
    mean_deg = total_edges / n
    hubs = sum(1 for d in degrees if d >= 100)
    print(
        f"{profile:>8s}  N={n}  E={total_edges}  "
        f"max_deg={max_deg}  mean_deg={mean_deg:.2f}  hubs(>=100)={hubs}  "
        f"-> {out_path}",
        file=sys.stderr,
    )

def usage(argv0):
    print(f"usage: {argv0} SIZE PROFILE OUTPUT", file=sys.stderr)
    print(f"  PROFILE in {list(PROFILES)}", file=sys.stderr)
    sys.exit(2)

def main(argv):
    if len(argv) != 4:
        usage(argv[0])
    n = int(argv[1])
    profile = argv[2]
    out = argv[3]
    generate(n, profile, out)

if __name__ == "__main__":
    main(sys.argv)
