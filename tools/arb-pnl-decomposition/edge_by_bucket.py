"""Execution edge of CE band hedges by order type and fill age (EXP-2026-10-22-arbitrageur-pnl, experiment E1).

Run on the dev host:  python3 -W ignore - START_MS END_MS < edge_by_bucket.py
Edge of a fill = s * (p - mid_venue(t)) * q with s=+1 for SELL, -1 for BUY: positive means the fill price is better than the
venue mid at the report time. Only */USDT pairs (BTC, ETH, SOL) are used, all agents (A_* arbitrageurs, T_* translators).
Buckets: 'instant' = fill within 2 s of emission (matched against prints already in the buffer; with CE_SIM_FORWARD_ONLY=1 these
must vanish) and 'followup' = later fills of resting orders. t-statistic across 15-minute blocks of notional-weighted bps.
"""
import sys, subprocess, bisect, collections, math

START, END = int(sys.argv[1]), int(sys.argv[2])
MAX_STALE_MS = 120000
BLOCK_MS = 900000


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, check=True).stdout


pg = sh("docker exec -i infra-postgres-1 psql -U cex -d cex -At -F '\t' -c "
        "\"SELECT client_order_id, split_part(hedge_flow_id,'|',3), venue_id, symbol, side, order_type "
        "FROM child_orders WHERE hedge_flow_id LIKE 'ce|band|%' AND created_at > to_timestamp(" + str(START - 900000) + "/1000.0)\"")
meta = {}
for line in pg.splitlines():
    p = line.split('\t')
    if len(p) == 6 and p[3].endswith('/USDT'):
        meta[p[0]] = dict(agent=p[1], venue=p[2], symbol=p[3], side=p[4], otype=p[5])

ch = sh("docker exec -i infra-clickhouse-1 clickhouse-client -q \"SELECT intent_id, event_time_ms, filled_qty, average_price "
        "FROM default.execution_venue WHERE startsWith(intent_id, 'ce|band|') AND event_time_ms > %d AND event_time_ms <= %d "
        "ORDER BY intent_id, event_time_ms FORMAT TSV\"" % (START - 900000, END))
fills = []
prev = {}
for line in ch.splitlines():
    p = line.split('\t')
    iid, t, cum, px = p[0], int(p[1]), float(p[2]), float(p[3])
    m = meta.get(iid)
    if not m:
        continue
    last = prev.get(iid, 0.0)
    if cum > last + 1e-12:
        d = cum - last
        prev[iid] = cum
        if px <= 0 or t < START:
            continue
        try:
            t0 = int(iid.rsplit('|', 1)[1])
        except Exception:
            t0 = t
        fills.append(dict(kind=m['agent'][0], venue=m['venue'], symbol=m['symbol'], side=m['side'], otype=m['otype'],
                          t=t, age_s=(t - t0) / 1000.0, q=d, p=px))

syms = sorted(set(f['symbol'] for f in fills))
if not fills:
    print('no fills in window')
    sys.exit(0)
mid_raw = sh("docker exec -i infra-clickhouse-1 clickhouse-client -q \"SELECT venue_id, symbol, event_time_ms, mid_price "
             "FROM default.venue_snapshots WHERE symbol IN (%s) AND event_time_ms >= %d AND event_time_ms <= %d AND mid_price > 0 "
             "ORDER BY venue_id, symbol, event_time_ms FORMAT TSV\"" % (",".join("'%s'" % s for s in syms), START - 300000, END + 60000))
ts = collections.defaultdict(list)
mv = collections.defaultdict(list)
for line in mid_raw.splitlines():
    v, s, t, m = line.split('\t')
    ts[(v, s)].append(int(t))
    mv[(v, s)].append(float(m))


def mid_at(v, s, t):
    k = (v, s)
    if k not in ts:
        return None
    i = bisect.bisect_right(ts[k], t) - 1
    if i < 0 or t - ts[k][i] > MAX_STALE_MS:
        return None
    return mv[k][i]


rows = []
skipped = 0
for f in fills:
    m = mid_at(f['venue'], f['symbol'], f['t'])
    if m is None:
        skipped += 1
        continue
    s = 1.0 if f['side'] == 'SELL' else -1.0
    f['edge'] = s * (f['p'] - m) * f['q']
    f['notional'] = f['p'] * f['q']
    f['bucket'] = 'instant' if f['age_s'] < 2 else 'followup'
    rows.append(f)


def tstat(xs):
    n = len(xs)
    if n < 4:
        return None
    m = sum(xs) / n
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1))
    return m / (sd / math.sqrt(n)) if sd > 0 else None


def line(label, pred):
    sel = [r for r in rows if pred(r)]
    nt = sum(r['notional'] for r in sel)
    if not sel or nt <= 0:
        print('  %-34s n=%5d notional=%11.0f' % (label, len(sel), nt))
        return
    e = sum(r['edge'] for r in sel)
    blocks = collections.defaultdict(lambda: [0.0, 0.0])
    for r in sel:
        b = r['t'] // BLOCK_MS
        blocks[b][0] += r['edge']
        blocks[b][1] += r['notional']
    bps = [1e4 * v[0] / v[1] for v in blocks.values() if v[1] > 0]
    t = tstat(bps)
    print('  %-34s n=%5d notional=%11.0f edge=%9.1f  %6.2f bps  blocks=%2d  t=%s' % (
        label, len(sel), nt, e, 1e4 * e / nt, len(bps), 'n/a' if t is None else '%.2f' % t))


print('window %d..%d ms, fills=%d, no-mid skipped=%d' % (START, END, len(rows), skipped))
for kind, kname in (('', 'ALL'), ('A', 'arbitrageurs'), ('T', 'translators')):
    sel = (lambda r, k=kind: (k == '' or r['kind'] == k))
    print(kname)
    line('LIMIT all', lambda r, s=sel: s(r) and r['otype'] != 'MARKET')
    line('LIMIT instant (<2s)', lambda r, s=sel: s(r) and r['otype'] != 'MARKET' and r['bucket'] == 'instant')
    line('LIMIT followup (>=2s)', lambda r, s=sel: s(r) and r['otype'] != 'MARKET' and r['bucket'] == 'followup')
    line('MARKET all', lambda r, s=sel: s(r) and r['otype'] == 'MARKET')
print('ALL agents, LIMIT by venue')
for v in sorted(set(r['venue'] for r in rows)):
    line('LIMIT ' + v, lambda r, v=v: r['venue'] == v and r['otype'] != 'MARKET')

print('ALL agents, LIMIT fills by order age at fill time (clean forward fills are the late ones: age beyond one read gap)')
for lo, hi in ((0, 2), (2, 10), (10, 20), (20, 40), (40, 80), (80, 400)):
    line('LIMIT age %d-%d s' % (lo, hi), lambda r, lo=lo, hi=hi: r['otype'] != 'MARKET' and lo <= r['age_s'] < hi)


def ci_line(label, pred):
    sel = [r for r in rows if pred(r)]
    if len(sel) < 5:
        print('  %-34s n=%5d (too few)' % (label, len(sel)))
        return
    w = [r['notional'] for r in sel]
    x = [1e4 * r['edge'] / r['notional'] for r in sel]
    sw = sum(w)
    mean = sum(a * b for a, b in zip(w, x)) / sw
    se = math.sqrt(sum((a * (b - mean)) ** 2 for a, b in zip(w, x))) / sw
    print('  %-34s n=%5d notional=%10.0f  mean=%6.2f bps  95%% CI [%6.2f, %6.2f]' % (
        label, len(sel), sw, mean, mean - 1.96 * se, mean + 1.96 * se))


print('clean forward fills (order age >= 20 s), LIMIT, notional-weighted mean with fill-level 95% CI')
ci_line('all agents', lambda r: r['otype'] != 'MARKET' and r['age_s'] >= 20)
ci_line('arbitrageurs', lambda r: r['otype'] != 'MARKET' and r['age_s'] >= 20 and r['kind'] == 'A')
for v in sorted(set(r['venue'] for r in rows)):
    ci_line('venue ' + v, lambda r, v=v: r['otype'] != 'MARKET' and r['age_s'] >= 20 and r['venue'] == v)
print('for comparison, instant fills (age < 2 s), LIMIT')
ci_line('all agents', lambda r: r['otype'] != 'MARKET' and r['age_s'] < 2)
