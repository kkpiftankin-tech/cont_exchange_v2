"""Decomposition of the CE arbitrageurs' external-hedge P&L (EXP-2026-10-22-arbitrageur-pnl).

Run on the dev host:  python3 -W ignore - < tools/arb-pnl-decomposition/decompose.py
Reads per-tick fills from ClickHouse default.execution_venue (cumulative filled_qty -> deltas, tick VWAP),
order side/venue/type from PostgreSQL child_orders, own-venue mid prices from default.venue_snapshots.
Exact identity per agent:  total = sum s*(p - mid_venue)*q      (execution edge)
                                 + sum s*(mid_venue - mid_ref)*q (cross-venue basis)
                                 + sum I_{k-1}*(ref_k - ref_{k-1}) (inventory drift: net position x price move).
The window starts at the trade-stats reset (RESET below); change it for another period.
"""
import subprocess, bisect, collections, calendar, datetime, sys

# Window: since the trade-stats reset (same as the panel)
RESET = calendar.timegm(datetime.datetime(2026, 10, 19, 13, 46, 53).timetuple()) * 1000
MAX_STALE_MS = 120000


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, check=True).stdout


# 1) order metadata (side, venue, type) from PG
pg = sh("docker exec -i infra-postgres-1 psql -U cex -d cex -At -F '\t' -c "
        "\"SELECT client_order_id, split_part(hedge_flow_id,'|',3), venue_id, symbol, side, order_type "
        "FROM child_orders WHERE hedge_flow_id LIKE 'ce|band|A\\_%' AND created_at > '2026-10-19 13:46:53+00'\"")
meta = {}
for line in pg.splitlines():
    p = line.split('\t')
    if len(p) == 6:
        meta[p[0]] = dict(agent=p[1], venue=p[2], symbol=p[3], side=p[4], otype=p[5])

# 2) per-tick fills from ClickHouse execution reports (cumulative filled_qty -> deltas)
ch = sh("docker exec -i infra-clickhouse-1 clickhouse-client -q \"SELECT intent_id, event_time_ms, filled_qty, average_price "
        "FROM default.execution_venue WHERE startsWith(intent_id, 'ce|band|A_') AND event_time_ms > %d "
        "ORDER BY intent_id, event_time_ms FORMAT TSV\"" % RESET)
fills = []
prev = {}
zero_px_ticks = 0
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
        if px <= 0:
            zero_px_ticks += 1
            continue
        try:
            t0 = int(iid.rsplit('|', 1)[1])
        except Exception:
            t0 = t
        fills.append(dict(agent=m['agent'], venue=m['venue'], symbol=m['symbol'], side=m['side'], otype=m['otype'],
                          t=t, age_s=(t - t0) / 1000.0, q=d, p=px))

# 3) mid price series per (venue, symbol)
assets = sorted(set(f['symbol'] for f in fills))
sym_list = ",".join("'%s'" % s for s in assets)
mid_raw = sh("docker exec -i infra-clickhouse-1 clickhouse-client -q \"SELECT venue_id, symbol, event_time_ms, mid_price "
             "FROM default.venue_snapshots WHERE symbol IN (%s) AND event_time_ms >= %d AND mid_price > 0 "
             "ORDER BY venue_id, symbol, event_time_ms FORMAT TSV\"" % (sym_list, RESET - 300000))
ts = collections.defaultdict(list)
mv = collections.defaultdict(list)
for line in mid_raw.splitlines():
    v, s, t, m = line.split('\t')
    ts[(v, s)].append(int(t))
    mv[(v, s)].append(float(m))
now_ms = max(max(x) for x in ts.values())


def mid_at(v, s, t):
    k = (v, s)
    if k not in ts:
        return None
    i = bisect.bisect_right(ts[k], t) - 1
    if i < 0 or t - ts[k][i] > MAX_STALE_MS:
        return None
    return mv[k][i]


def ref_at(s, t):
    for v in ('binance', 'okx', 'coinbase', 'kraken'):
        m = mid_at(v, s, t)
        if m is not None:
            return m
    return None


# 4) decomposition: total = exec edge vs own-venue mid + cross-venue basis + inventory drift (exact identity)
by_agent = collections.defaultdict(list)
skipped = 0
for f in fills:
    mvn = mid_at(f['venue'], f['symbol'], f['t'])
    rf = ref_at(f['symbol'], f['t'])
    if mvn is None or rf is None:
        skipped += 1
        continue
    f['mid'] = mvn
    f['ref'] = rf
    by_agent[f['agent']].append(f)

rows = []
tot = collections.Counter()
by_venue_edge = collections.defaultdict(lambda: [0.0, 0.0])
by_kind_edge = collections.defaultdict(lambda: [0.0, 0.0])
for a, fl in by_agent.items():
    fl.sort(key=lambda x: x['t'])
    sym = fl[0]['symbol']
    inv = 0.0
    prev_ref = None
    e1 = e2 = e3 = 0.0
    gross = 0.0
    cash = 0.0
    B = S = bn = sn = bn_mid = sn_mid = 0.0
    for f in fl:
        s = 1.0 if f['side'] == 'SELL' else -1.0
        q, p = f['q'], f['p']
        if prev_ref is not None:
            e3 += inv * (f['ref'] - prev_ref)
        e1 += s * (p - f['mid']) * q
        e2 += s * (f['mid'] - f['ref']) * q
        inv += -s * q
        prev_ref = f['ref']
        gross += p * q
        cash += s * p * q
        if s > 0:
            S += q; sn += q * p; sn_mid += q * f['mid']
        else:
            B += q; bn += q * p; bn_mid += q * f['mid']
        k = (f['venue'], f['otype'])
        by_kind_edge[k][0] += s * (p - f['mid']) * q
        by_kind_edge[k][1] += p * q
        kk = 'instant(<2s)' if f['age_s'] < 2 else 'followup(>=2s)'
        by_venue_edge[(f['venue'], kk)][0] += s * (p - f['mid']) * q
        by_venue_edge[(f['venue'], kk)][1] += p * q
    ref_now = ref_at(sym, now_ms)
    e3 += inv * (ref_now - prev_ref)
    total_direct = cash + inv * ref_now
    r_fmt = min(B, S) * (sn / S - bn / B) if B > 0 and S > 0 else 0.0
    r_placebo = min(B, S) * (sn_mid / S - bn_mid / B) if B > 0 and S > 0 else 0.0
    rows.append((a, len(fl), gross, e1, e2, e3, e1 + e2 + e3, total_direct, r_fmt, r_placebo, inv))
    tot['n'] += len(fl)
    tot['gross'] += gross
    tot['e1'] += e1
    tot['e2'] += e2
    tot['e3'] += e3
    tot['total'] += e1 + e2 + e3
    tot['direct'] += total_direct

rows.sort(key=lambda r: r[6])
print('window since %s UTC, fills used=%d skipped(no mid)=%d zero_px_ticks=%d' % (
    datetime.datetime.utcfromtimestamp(RESET / 1000), tot['n'], skipped, zero_px_ticks))
print('%-22s %5s %9s | %8s %8s %8s | %8s %8s | %8s %8s %9s' % (
    'agent', 'fills', 'gross$', 'exec', 'basis', 'invent', 'sum', 'direct', 'R_panel', 'R_placeb', 'net_inv'))
for r in rows:
    print('%-22s %5d %9.0f | %8.1f %8.1f %8.1f | %8.1f %8.1f | %8.1f %8.1f %9.4f' % r)
print('%-22s %5d %9.0f | %8.1f %8.1f %8.1f | %8.1f %8.1f' % (
    'TOTAL', tot['n'], tot['gross'], tot['e1'], tot['e2'], tot['e3'], tot['total'], tot['direct']))
print('exec edge in bps of notional: %.2f ; basis: %.2f ; inventory: %.2f' % (
    1e4 * tot['e1'] / tot['gross'], 1e4 * tot['e2'] / tot['gross'], 1e4 * tot['e3'] / tot['gross']))
print()
print('exec edge by venue and fill kind (bps of notional):')
for k in sorted(by_venue_edge):
    e, n = by_venue_edge[k]
    print('  %-10s %-15s edge=%9.1f notional=%10.0f  %7.2f bps' % (k[0], k[1], e, n, 1e4 * e / n if n else 0))
print('exec edge by venue and order type (bps):')
for k in sorted(by_kind_edge):
    e, n = by_kind_edge[k]
    print('  %-10s %-8s edge=%9.1f notional=%10.0f  %7.2f bps' % (k[0], k[1], e, n, 1e4 * e / n if n else 0))

# ---- significance of the inventory term: hourly blocks (sum over agents), t-stat across hours
import math
hour = collections.defaultdict(lambda: [0.0, 0.0, 0.0])   # exec, basis, inventory per UTC hour
day = collections.defaultdict(lambda: [0.0, 0.0, 0.0])
absinv = []
for a, fl in by_agent.items():
    inv = 0.0
    prev_ref = None
    for f in fl:
        s = 1.0 if f['side'] == 'SELL' else -1.0
        h = f['t'] // 3600000
        d = f['t'] // 86400000
        if prev_ref is not None:
            x = inv * (f['ref'] - prev_ref)
            hour[h][2] += x
            day[d][2] += x
        e = s * (f['p'] - f['mid']) * f['q']
        b = s * (f['mid'] - f['ref']) * f['q']
        hour[h][0] += e; hour[h][1] += b
        day[d][0] += e; day[d][1] += b
        inv += -s * f['q']
        prev_ref = f['ref']
        absinv.append(abs(inv) * f['ref'])


def tstat(xs):
    n = len(xs)
    m = sum(xs) / n
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1))
    return n, m, sd, m / (sd / math.sqrt(n)) if sd > 0 else 0.0


hrs = sorted(hour)
for i, name in enumerate(('exec edge', 'basis', 'inventory drift')):
    n, m, sd, t = tstat([hour[h][i] for h in hrs])
    pos = sum(1 for h in hrs if hour[h][i] > 0)
    print('hourly %-16s n=%d mean=%7.2f sd=%7.2f t=%6.2f  positive_hours=%d/%d' % (name, n, m, sd, t, pos, n))
print('daily totals (UTC day): exec / basis / inventory')
for d in sorted(day):
    print('  %s  %8.1f %8.1f %8.1f' % (datetime.datetime.utcfromtimestamp(d * 86400), day[d][0], day[d][1], day[d][2]))
absinv.sort()
print('mean |net inventory| per fill, USD: %.0f ; median %.0f ; p90 %.0f' % (
    sum(absinv) / len(absinv), absinv[len(absinv) // 2], absinv[int(len(absinv) * 0.9)]))

# ---- counterfactual replay: accept a fill only while |net inventory| stays within cap (USD); skipped volume is simply not traded
print()
print('replay with inventory cap (USD): retained notional, exec+basis, inventory drift, total, hourly sd of total, mean/sd')
for cap in (None, 20000, 10000, 5000, 2000, 1000, 500):
    hrs_tot = collections.defaultdict(float)
    ex = ba = iv = notional = 0.0
    for a, fl in by_agent.items():
        inv = 0.0
        prev_ref = None
        for f in fl:
            s = 1.0 if f['side'] == 'SELL' else -1.0
            q = f['q']
            if cap is not None:
                cu = cap / f['ref']
                q_max = (inv + cu) if s > 0 else (cu - inv)   # largest fill keeping |inventory| <= cap
                q = min(q, max(0.0, q_max))
                if q <= 1e-15:
                    continue
            h = f['t'] // 3600000
            x = 0.0
            if prev_ref is not None:
                x = inv * (f['ref'] - prev_ref)
            e = s * (f['p'] - f['mid']) * q
            b = s * (f['mid'] - f['ref']) * q
            ex += e; ba += b; iv += x
            hrs_tot[h] += e + b + x
            inv += -s * q
            prev_ref = f['ref']
            notional += f['p'] * q
        if prev_ref is not None:
            ref_now_a = ref_at(fl[0]['symbol'], now_ms)
            iv += inv * (ref_now_a - prev_ref)
            hrs_tot[now_ms // 3600000] += inv * (ref_now_a - prev_ref)
    vals = [hrs_tot[h] for h in sorted(hrs_tot)]
    n, m, sd, t = tstat(vals)
    print('cap=%-7s notional=%5.1f%%  exec+basis=%8.1f  inventory=%8.1f  total=%8.1f  hourly_sd=%6.1f  mean/sd=%5.2f' % (
        'none' if cap is None else cap, 100.0 * notional / tot['gross'], ex + ba, iv, ex + ba + iv, sd, m / sd if sd else 0))

# ---- per-day volume and edge: is the drop in earnings a drop in volume or in margin?
print()
print('per UTC day: fills, notional, exec edge (bps), maker share of notional, taker (MARKET) edge bps')
dd = collections.defaultdict(lambda: dict(n=0, nt=0.0, e=0.0, lim_nt=0.0, mk_nt=0.0, mk_e=0.0, lim_e=0.0))
for a, fl in by_agent.items():
    for f in fl:
        d = f['t'] // 86400000
        s = 1.0 if f['side'] == 'SELL' else -1.0
        e = s * (f['p'] - f['mid']) * f['q']
        nt = f['p'] * f['q']
        x = dd[d]
        x['n'] += 1; x['nt'] += nt; x['e'] += e
        if f['otype'] == 'MARKET':
            x['mk_nt'] += nt; x['mk_e'] += e
        else:
            x['lim_nt'] += nt; x['lim_e'] += e
for d in sorted(dd):
    x = dd[d]
    print('  %s fills=%4d notional=%9.0f exec=%7.1f (%5.2f bps)  limit: %5.2f bps on %9.0f | market: %5.2f bps on %8.0f' % (
        datetime.datetime.utcfromtimestamp(d * 86400).strftime('%m-%d'), x['n'], x['nt'], x['e'], 1e4 * x['e'] / x['nt'],
        1e4 * x['lim_e'] / x['lim_nt'] if x['lim_nt'] else 0, x['lim_nt'], 1e4 * x['mk_e'] / x['mk_nt'] if x['mk_nt'] else 0, x['mk_nt']))
