"""Checks live CE clearing tacts against the theory (EXP-2026-10-22-ce-theory-conformity).

Input: JSON lines from ce_clearing_sampler.py.  Run:  python3 ce_theory_check.py SAMPLES.jsonl

A. Engine vs theory: with zero dead zones the clearing is the linear system  Lambda x = beta  (Lambda = B G B^T, beta = B G s,
   one pinned node), flows f_e = alpha_e (anchor_e - (x_from - x_to)).  We re-solve every tact and compare with the engine's flows.
B. Clearing surplus of every agent per tact  S_e = f_e * d_e / 2  (USDT; f in k-USDT, d = anchor - rate in permille) -- Budish et al. /
   'Krivye kotirovaniya' sec. 5.2: the surplus of a cycle is eps*J/2 and is shared in proportion to impacts 1/alpha.
C. Counterfactual without the inventory feedback (anchor_raw instead of anchor): how much surplus the position feedback removes.
D. Position projection: node balance of the position deltas that the ledger receives (translator +f at 'from' node, link legs
   src -f / dst +f as in ce_position_projection.hpp) versus the clearing balance sum(B f) = 0.
"""
import json, sys, math, collections


def solve(A, b):
    n = len(b)
    M = [r[:] + [b[i]] for i, r in enumerate(A)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(M[r][c]))
        M[c], M[p] = M[p], M[c]
        if abs(M[c][c]) < 1e-14:
            return None
        for r in range(n):
            if r == c:
                continue
            k = M[r][c] / M[c][c]
            for j in range(c, n + 1):
                M[r][j] -= k * M[c][j]
    return [M[i][n] / M[i][i] for i in range(n)]


def clear(node_ids, pinned, edges, use_raw=False):
    """edges: (name, kind, u, v, anchor, anchor_raw, depth). returns flows list."""
    idx = {n: i for i, n in enumerate(node_ids)}
    n = len(node_ids)
    L = [[0.0] * n for _ in range(n)]
    beta = [0.0] * n
    for (_, _, u, v, a, ar, d) in edges:
        s = ar if use_raw else a
        i, j = idx[u], idx[v]
        L[i][i] += d; L[j][j] += d; L[i][j] -= d; L[j][i] -= d
        beta[i] += d * s; beta[j] -= d * s
    A = [r[:] for r in L]
    b = beta[:]
    for pn, pv in pinned.items():
        g = idx[pn]
        for u in range(n):
            b[u] -= A[u][g] * pv
        for u in range(n):
            A[u][g] = 0.0; A[g][u] = 0.0
        A[g][g] = 1.0; b[g] = pv
    x = solve(A, b)
    flows = []
    for (_, _, u, v, a, ar, d) in edges:
        s = ar if use_raw else a
        flows.append(d * (s - (x[idx[u]] - x[idx[v]])))
    return x, flows


samples = []
for l in open(sys.argv[1]):
    try:
        samples.append(json.loads(l))
    except ValueError:
        pass   # last line may be partial while the sampler is still writing
print('tacts sampled: %d, window %.1f min' % (len(samples), (samples[-1]['t'] - samples[0]['t']) / 60000.0))

max_diff = 0.0
n_checked = 0
n_dz = 0
S = collections.defaultdict(float)       # actual surplus per agent, USDT
S_raw = collections.defaultdict(float)   # surplus without inventory feedback
kinds = {}
node_imb_clear = collections.defaultdict(float)
node_imb_proj = collections.defaultdict(float)
node_gross = collections.defaultdict(float)
flow_abs = collections.defaultdict(float)

for rec in samples:
    nodes = rec['nodes']
    node_ids = list(nodes.keys())
    pinned = {k: v[0] for k, v in nodes.items() if v[1]}
    edges = [(e[0], e[1], e[2], e[3], e[4], e[5], e[6]) for e in rec['edges'] if e[6] and e[6] > 0]
    if any((e[7] or 0) > 0 for e in rec['edges']):
        n_dz += 1
    if not edges or not pinned:
        continue
    for e in rec['edges']:
        kinds[e[0]] = e[1]
    x, f_re = clear(node_ids, pinned, edges)
    for (name, kind, u, v, a, ar, d), fr, e in zip(edges, f_re, [e for e in rec['edges'] if e[6] and e[6] > 0]):
        if (e[7] or 0) == 0 and e[8] is not None:
            max_diff = max(max_diff, abs(fr - e[8]))
            n_checked += 1
    # B. surplus (USDT): S = f * d / 2 with d = anchor - rate  (flow = depth * d)
    for (name, kind, u, v, a, ar, d), fr in zip(edges, f_re):
        dev = fr / d if d else 0.0
        S[name] += 0.5 * fr * dev
        flow_abs[name] += abs(fr)
    # C. no inventory feedback
    x_raw, f_raw = clear(node_ids, pinned, edges, use_raw=True)
    for (name, kind, u, v, a, ar, d), fr in zip(edges, f_raw):
        dev = fr / d if d else 0.0
        S_raw[name] += 0.5 * fr * dev
    # D. projections
    for (name, kind, u, v, a, ar, d), fr, e in zip(edges, f_re, [e for e in rec['edges'] if e[6] and e[6] > 0]):
        flow = e[8] if e[8] is not None else fr
        node_imb_clear[u] += flow; node_imb_clear[v] -= flow
        if kind == 'arbitrageur':
            node_imb_proj[u] += -flow; node_imb_proj[v] += flow
        else:
            node_imb_proj[u] += flow; node_imb_proj[v] -= flow
        node_gross[u] += abs(flow); node_gross[v] += abs(flow)

print('\nA. engine vs linear-system solution (zero dead zones): tacts with dead zone > 0: %d; edges compared %d; max |f_engine - f_theory| = %.3e k-USDT' % (n_dz, n_checked, max_diff))

dur_h = (samples[-1]['t'] - samples[0]['t']) / 3600000.0
print('\nB/C. clearing surplus per agent (USDT) over the window and extrapolated per day (x%.1f)' % (24.0 / dur_h))
print('%-26s %-11s %10s %10s %12s %12s' % ('agent', 'kind', 'S window', 'S per day', 'S no-skew/d', 'skew removes'))
tot = collections.defaultdict(float)
for name in sorted(S, key=lambda k: -S[k]):
    k = kinds.get(name, '?')
    per_day = S[name] * 24.0 / dur_h
    raw_day = S_raw[name] * 24.0 / dur_h
    tot[k] += per_day
    tot[k + '_raw'] += raw_day
    rem = 100.0 * (1 - S[name] / S_raw[name]) if S_raw[name] > 1e-12 else 0.0
    print('%-26s %-11s %10.3f %10.1f %12.1f %11.0f%%' % (name, k, S[name], per_day, raw_day, rem))
print('TOTAL per day  translators=%.1f (no-skew %.1f)  arbitrageurs=%.1f (no-skew %.1f)' % (
    tot['translator'], tot['translator_raw'], tot['arbitrageur'], tot['arbitrageur_raw']))

print('\nD. node balance: clearing sum(B f) vs projected position deltas (translator +f at from, link src -f / dst +f)')
print('%-14s %12s %12s %12s' % ('node', 'clear', 'projected', 'gross flow'))
for nd in sorted(node_gross, key=lambda k: -abs(node_imb_proj[k]))[:10]:
    print('%-14s %12.3f %12.3f %12.1f' % (nd, node_imb_clear[nd], node_imb_proj[nd], node_gross[nd]))
