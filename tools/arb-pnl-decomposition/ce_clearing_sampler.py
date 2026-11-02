"""Samples the last CE clearing snapshot (BFF /api/vector-clearing/ce-clearing) about once a second and stores one JSON line per
distinct tact. Used by EXP-2026-10-22-ce-theory-conformity to compute the clearing surplus of every agent from real tacts.

Run on the dev host:  python3 ce_clearing_sampler.py SECONDS OUTFILE
"""
import json, sys, time, urllib.request

seconds = int(sys.argv[1])
out = sys.argv[2]
url = 'http://localhost:8090/api/vector-clearing/ce-clearing'
epoch_url = 'http://localhost:8090/api/vector-clearing/epochs?limit=1'
last = None
# Метка сброса (ADR-090): каждая записанная строка помечается номером текущей эпохи, чтобы разбор файла не смешивал эксперименты.
# Номер обновляется не чаще раза в 5 секунд; None — метки недоступны (старый BFF).
cur_epoch, cur_epoch_at = None, 0.0


def current_epoch():
    global cur_epoch, cur_epoch_at
    if time.time() - cur_epoch_at > 5:
        cur_epoch_at = time.time()
        try:
            e = json.load(urllib.request.urlopen(epoch_url, timeout=3))
            cur_epoch = (e.get('current') or {}).get('epoch') if e.get('supported') else None
        except Exception:
            pass   # прежнее значение: кратковременный сбой не должен менять метку записей
    return cur_epoch


end = time.time() + seconds
with open(out, 'a') as fh:
    while time.time() < end:
        try:
            d = json.load(urllib.request.urlopen(url, timeout=8))
            s = d.get('solve') or {}
            b = s.get('batchId')
            if b and b != last:
                last = b
                rec = {
                    't': s.get('eventTimeMs'), 'epoch': current_epoch(), 'batch': b, 'conv': s.get('converged'), 'imb': s.get('maxImbalance'),
                    'nodes': {n['id']: [n.get('potential_pm'), bool(n.get('pinned'))] for n in s.get('nodes', [])},
                    'edges': [[e['name'], e['kind'], e['from'], e['to'], e.get('anchor_pm'), e.get('anchor_raw_pm'), e.get('depth'),
                               e.get('dead_zone_pm'), e.get('flow_kusdt'), e.get('deviation_pm'), e.get('state')]
                              for e in s.get('edges', [])],
                }
                fh.write(json.dumps(rec) + '\n')
                fh.flush()
        except Exception:
            pass
        time.sleep(0.8)
