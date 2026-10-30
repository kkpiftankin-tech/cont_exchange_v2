# -*- coding: utf-8 -*-
"""
Сравнение стратегий опроса бирж по REST на модели с измеренными задержками и внесёнными сбоями.

Что МОДЕЛЬ, а что ИЗМЕРЕНИЕ:
  измерено (dev-хост, 2026-10-15): медиана задержки запроса при keep-alive — binance 0.285 с, okx 0.33, kraken 0.055, coinbase 0.05;
  принято как допущение: «настоящие» лимиты бирж (binance 20 запр/с, okx 10, coinbase 10, kraken 1.5), форма хвоста задержек,
  параметры сценариев сбоев. Результаты — сравнение стратегий между собой, не прогноз абсолютных цифр.

Стратегии:
  S0  как сейчас: общий цикл (5 с паузы после самой медленной биржи), пары по очереди, отказ при нехватке токена,
      предохранитель на биржу (3 ошибки за 60 с -> пауза 10 с, в полуоткрытом первая ошибка открывает снова).
  S2  ИТОГОВАЯ (ADR-078): фиксированный период на пару со сдвигами фаз (без накопления пропущенных тиков), пул из 3
      запросов на биржу, пейсинг по бюджету 50 % лимита, дедлайн 0,8 с, до 3 повторов в пределах бюджета тика,
      карантин пары после 5 подряд неудач (проба 30 -> 300 с), авария биржи = ≥3 пар с сетевым сбоем за 10 с ->
      пауза биржи с пробами 2 -> 10 с (карантин пар не копится), 429 -> пауза + AIMD (период x2, возврат x0,9/мин).
      Период T = max(3 с, N_пар / (0,5 * лимит биржи)): binance/okx/coinbase 3 с, kraken 7 с.
  S3  WebSocket + выборка раз в секунду (справочно: толчковые обновления; допущения по обрывам — в sim_ws).

Запуск: python3 polling_sim.py            — таблицы по сценариям (человекочитаемо)
        python3 polling_sim.py --markdown — те же данные в виде markdown-таблиц для документа
"""
import heapq
import math
import random
import statistics as st
from collections import defaultdict

PAIRS = ['BTC/USDT', 'ETH/USDT', 'SOL/USDT', 'ETH/BTC', 'SOL/BTC']
VENUES = {
    #           медиана задержки, реальный лимит (запр/с), реальный burst, лимит клиента S0 (rps, burst)
    'binance':  dict(lat=0.285, real_rps=20.0, real_burst=40, cfg_rps=8.0, cfg_burst=16),
    'okx':      dict(lat=0.330, real_rps=10.0, real_burst=20, cfg_rps=8.0, cfg_burst=16),
    'coinbase': dict(lat=0.050, real_rps=10.0, real_burst=20, cfg_rps=8.0, cfg_burst=16),
    'kraken':   dict(lat=0.055, real_rps=1.5,  real_burst=6,  cfg_rps=2.0, cfg_burst=4),
}
T_END = 1200.0
WARM = 120.0


class Bucket:
    def __init__(self, rps, burst):
        self.rps, self.cap, self.tokens, self.t = rps, burst, burst, 0.0

    def refill(self, t):
        if t > self.t:
            self.tokens = min(self.cap, self.tokens + (t - self.t) * self.rps)
            self.t = t

    def take(self, t):
        self.refill(t)
        if self.tokens >= 1.0:
            self.tokens -= 1.0
            return True
        return False

    def wait_for(self, t):
        """время до появления токена (не взяв его)"""
        self.refill(t)
        return 0.0 if self.tokens >= 1.0 else (1.0 - self.tokens) / self.rps


class Scenario:
    def __init__(self, name, flaky400=None, timeouts=None, real_override=None, outage=None):
        self.name = name
        self.flaky400 = flaky400 or {}        # (venue, pair) -> вероятность 400
        self.timeouts = timeouts or []        # (t0, t1, p) для всех бирж
        self.real_override = real_override or {}
        self.outage = outage or {}            # venue -> (t0, t1)


SCENARIOS = [
    Scenario('N  норма'),
    Scenario('F  Coinbase: 400 вразнобой на 2 парах (как User-Agent)',
             flaky400={('coinbase', 'ETH/BTC'): 0.4, ('coinbase', 'SOL/BTC'): 0.4}),
    Scenario('L  всплеск таймаутов 60 с (30 % запросов)', timeouts=[(400, 460, 0.30)]),
    Scenario('R  Kraken реально строже (1 запр/с)', real_override={'kraken': (1.0, 4)}),
    Scenario('O  авария OKX 90 с', outage={'okx': (500, 590)}),
    Scenario('X  всё вместе (F + L + R + O)',
             flaky400={('coinbase', 'ETH/BTC'): 0.4, ('coinbase', 'SOL/BTC'): 0.4},
             timeouts=[(400, 460, 0.30)], real_override={'kraken': (1.0, 4)}, outage={'okx': (500, 590)}),
]


class Server:
    """Сервер биржи: лимит запросов (при превышении 429 и штраф 5 с), задержки, сбои сценария."""

    def __init__(self, venue, scn, rnd):
        v = VENUES[venue]
        self.venue, self.lat, self.scn, self.rnd = venue, v['lat'], scn, rnd
        rps, burst = scn.real_override.get(venue, (v['real_rps'], v['real_burst']))
        self.bucket = Bucket(rps, burst)
        self.ban_until = 0.0
        self.n_req = 0
        self.n_429 = 0

    def request(self, t, pair, deadline):
        """-> (исход, длительность). исходы: ok, timeout, conn, http400, http429, http5xx"""
        self.n_req += 1
        o = self.scn.outage.get(self.venue)
        if o and o[0] <= t < o[1]:
            return 'conn', min(1.0, deadline)
        if t < self.ban_until:
            self.n_429 += 1
            return 'http429', self.lat
        if not self.bucket.take(t):
            self.ban_until = t + 5.0
            self.n_429 += 1
            return 'http429', self.lat
        for (t0, t1, p) in self.scn.timeouts:
            if t0 <= t < t1 and self.rnd.random() < p:
                return 'timeout', deadline
        p400 = self.scn.flaky400.get((self.venue, pair), 0.0)
        if p400 and self.rnd.random() < p400:
            return 'http400', self.lat
        lat = self.lat * math.exp(self.rnd.gauss(0.0, 0.35))
        if self.rnd.random() < 0.01:
            lat += 2.5                      # редкий длинный хвост
        if lat > deadline:
            return 'timeout', deadline
        return 'ok', lat


class Metrics:
    def __init__(self):
        self.updates = defaultdict(list)   # (venue, pair) -> моменты успешного обновления
        self.requests = defaultdict(int)
        self.n429 = defaultdict(int)
        self.skipped = defaultdict(int)    # потерянные тики/пропущенные пары

    def summarize(self, venue=None, outage=None):
        CAP = 120.0
        keys = [k for k in self.updates if venue is None or k[0] == venue]
        ratios, cvs, p50s, ages, rec = [], [], [], [], []
        for key in keys:
            ts = sorted(self.updates[key])
            g = sorted(b - a for a, b in zip(ts, ts[1:]) if a >= WARM)
            if len(g) > 3:
                med = g[len(g) // 2]
                ratios.append(g[min(len(g) - 1, int(0.99 * len(g)))] / med)
                cvs.append(st.pstdev(g) / st.mean(g))
                p50s.append(med)
            j, t = 0, WARM
            while t < T_END:
                while j + 1 < len(ts) and ts[j + 1] <= t:
                    j += 1
                last = ts[j] if ts and ts[j] <= t else None
                ages.append(min(CAP, t - last) if last is not None else CAP)
                t += 1.0
            if outage and key[0] == outage[0]:
                after = [x for x in ts if x >= outage[2]]
                rec.append((after[0] - outage[2]) if after else (T_END - outage[2]))
        ages.sort()
        q = lambda xs, p: xs[min(len(xs) - 1, int(p * len(xs)))] if xs else float('nan')
        nm = (T_END - WARM)
        builds = 60.0 * sum(len([x for x in self.updates[k] if x >= WARM]) for k in keys) / nm
        vs = [venue] if venue else list(VENUES)
        return dict(
            period=st.mean(p50s) if p50s else float('nan'),
            ratio=st.mean(ratios) if ratios else float('nan'),
            cv=st.mean(cvs) if cvs else float('nan'),
            age50=q(ages, 0.5), age99=q(ages, 0.99),
            stale15=100.0 * sum(1 for x in ages if x > 15.0) / max(1, len(ages)),
            builds=builds,
            reqs=sum(self.requests[v] for v in vs) / T_END,
            n429=sum(self.n429[v] for v in vs),
            rec=st.mean(rec) if rec else float('nan'),
        )


# ------------------------------------------------------------------ S0 как сейчас
def sim_s0(scn, seed):
    rnd = random.Random(seed)
    m = Metrics()
    srv = {v: Server(v, scn, rnd) for v in VENUES}
    own = {v: Bucket(VENUES[v]['cfg_rps'], VENUES[v]['cfg_burst']) for v in VENUES}
    errs = {v: [] for v in VENUES}
    open_until = {v: 0.0 for v in VENUES}
    half_open = {v: False for v in VENUES}
    last_ticker = defaultdict(lambda: -1e9)
    t = 0.0
    DEADLINE = 3.0
    while t < T_END:
        ends = []
        stalest = None
        for v in VENUES:
            tv = t
            for i, p in enumerate(PAIRS):
                if tv < open_until[v]:
                    m.skipped[v] += 1
                    continue
                if tv >= open_until[v] > 0 and not half_open[v] and open_until[v] > t - 1e9 and errs[v] and len(errs[v]) >= 3:
                    half_open[v] = True
                if not own[v].take(tv):
                    m.skipped[v] += 1
                    continue
                res, dur = srv[v].request(tv, p, DEADLINE)
                m.requests[v] += 1
                tv += dur
                # тикер раз в 60 с на пару (если есть токен)
                if res == 'ok' and tv - last_ticker[(v, p)] >= 60.0 and own[v].take(tv):
                    r2, d2 = srv[v].request(tv, p, DEADLINE)
                    m.requests[v] += 1
                    tv += d2
                    last_ticker[(v, p)] = tv
                if res == 'ok':
                    m.updates[(v, p)].append(tv)
                    errs[v] = []
                    half_open[v] = False
                else:
                    errs[v] = [x for x in errs[v] if tv - x < 60.0] + [tv]
                    if half_open[v] or len(errs[v]) >= 3:
                        open_until[v] = tv + 10.0
                        half_open[v] = False
            # лента сделок: один запрос за цикл на биржу
            if own[v].take(tv):
                r3, d3 = srv[v].request(tv, 'trades', DEADLINE)
                m.requests[v] += 1
                tv += d3
            ends.append(tv)
        t = max(ends) + 5.0
    for v in VENUES:
        m.n429[v] = srv[v].n_429
    return m


# ------------------------------------------------------------------ S1/S2 фиксированный период на пару
def base_period(v, tmin=3.0):
    real = VENUES[v]['real_rps']
    return max(tmin, math.ceil(len(PAIRS) / (0.5 * real)))


def sim_sched(scn, seed, workers=3, tmin=3.0, deadline=0.8, smart=True, max_retries=3, q_after=5):
    """smart=True — исправленная лестница: сетевые сбои (таймаут, обрыв) относятся к БИРЖЕ (пауза с короткой пробой),
    а карантин пары — только за сбои самой пары (4xx, неверные данные, таймаут при живых соседях)."""
    rnd = random.Random(seed)
    m = Metrics()
    for v in VENUES:
        srv = Server(v, scn, rnd)
        base_T = base_period(v, tmin)
        scale = 1.0
        own = Bucket(0.5 * VENUES[v]['real_rps'], max(2, int(0.5 * VENUES[v]['real_burst'] / 2)))
        due = {p: (i * base_T / len(PAIRS)) for i, p in enumerate(PAIRS)}
        fails = {p: 0 for p in PAIRS}
        quarantine = {p: 0.0 for p in PAIRS}
        q_step = {p: 30.0 for p in PAIRS}
        venue_pause = 0.0
        probe_step = 2.0
        backoff = 1.0
        last_429 = -1e9
        last_ok_any = -1e9
        last_ticker = defaultdict(lambda: -1e9)
        net_fail = []                      # (время, пара) сетевых сбоев за 10 с
        free = [0.0] * workers
        DEADLINE = deadline
        next_trades = 5.0
        while True:
            w = min(range(workers), key=lambda i: free[i])
            p = min(PAIRS, key=lambda x: due[x])
            T = base_T * scale
            start = max(free[w], due[p], venue_pause)
            if start >= T_END:
                break
            missed = 0
            while due[p] < start - T:
                due[p] += T
                missed += 1
            if missed:
                m.skipped[v] += missed
            if quarantine[p] > start:
                due[p] += T
                continue
            if scale > 1.0 and start - last_429 > 60.0:
                scale = max(1.0, scale * 0.9)
            wait = own.wait_for(start)
            if wait > 0.5 * T:
                due[p] += T
                m.skipped[v] += 1
                continue
            start += wait
            own.take(start)
            res, dur = srv.request(start, p, DEADLINE)
            m.requests[v] += 1
            t_end = start + dur
            if res in ('timeout', 'conn', 'http5xx', 'http400'):
                if res in ('timeout', 'conn'):
                    net_fail.append((t_end, p))
                tries = 0
                # повторы внутри бюджета тика (0,5·T), не больше max_retries; каждый повтор берёт токен
                while (res in ('timeout', 'conn', 'http5xx', 'http400') and tries < max_retries
                       and (t_end + 1.5 * VENUES[v]['lat']) < (due[p] + 0.5 * T) and own.take(t_end)):
                    tries += 1
                    res, dur2 = srv.request(t_end, p, DEADLINE)
                    m.requests[v] += 1
                    t_end += dur2
            if res == 'ok':
                m.updates[(v, p)].append(t_end)
                fails[p] = 0
                q_step[p] = 30.0
                probe_step = 2.0
                last_ok_any = t_end
                backoff = max(1.0, backoff * 0.5)
                if t_end - last_ticker[p] >= 60.0 and own.take(t_end):
                    _, d2 = srv.request(t_end, p, DEADLINE)
                    m.requests[v] += 1
                    t_end += d2
                    last_ticker[p] = t_end
            elif res == 'http429':
                last_429 = t_end
                venue_pause = t_end + backoff
                backoff = min(60.0, backoff * 2.0)
                scale = min(8.0, scale * 2.0)
            else:
                net_fail = [(x, q) for (x, q) in net_fail if t_end - x < 10.0]
                venue_wide = len({q for _, q in net_fail}) >= 3 and (t_end - last_ok_any) > 3.0
                if smart and venue_wide:
                    # авария биржи: пауза и короткие пробы (2 -> 4 -> 8 -> 10 с), карантин пар НЕ накапливаем
                    venue_pause = t_end + probe_step
                    probe_step = min(10.0, probe_step * 2.0)
                elif res in ('http400', 'http5xx') or (smart and res in ('timeout', 'conn') and (t_end - last_ok_any) < 5.0) or not smart:
                    fails[p] += 1
                    if fails[p] >= q_after:
                        quarantine[p] = t_end + q_step[p]
                        q_step[p] = min(300.0, q_step[p] * 2.0)
                        fails[p] = 0
            free[w] = t_end
            due[p] += T
            if t_end >= next_trades and own.take(t_end):
                _, d3 = srv.request(t_end, 'trades', DEADLINE)
                m.requests[v] += 1
                free[w] = t_end + d3
                next_trades = t_end + 9.0
        m.n429[v] = srv.n_429
    return m


# ------------------------------------------------------------------ S3 WebSocket + выборка 1 Гц (справочно)
def sim_ws(scn, seed):
    """Допущения (НЕ измерение): толчок по паре раз в 0,5 с; обрыв соединения — переподключение 2 с + REST-сверка 0,5 с;
    сценарии REST-сбоев (400, таймауты REST, лимиты) WebSocket не затрагивают; авария биржи прерывает поток."""
    rnd = random.Random(seed)
    m = Metrics()
    for v in VENUES:
        out = scn.outage.get(v)
        for p in PAIRS:
            t = rnd.random()
            while t < T_END:
                if out and out[0] <= t < out[1]:
                    t = out[1] + 2.5
                    continue
                m.updates[(v, p)].append(t)
                t += max(0.05, rnd.expovariate(1.0 / 0.5))
        m.requests[v] = 0
    # расчёт кривой не чаще 1 раза в секунду на пару (выборка), вычислительная нагрузка ниже
    for key, ts in list(m.updates.items()):
        sampled, last = [], -1e9
        for x in ts:
            if x - last >= 1.0:
                sampled.append(x)
                last = x
        m.updates[key] = sampled
    return m


SHOW = {'N': ('binance', 'okx', 'coinbase', 'kraken'), 'F': ('coinbase',), 'L': ('binance', 'kraken'),
        'R': ('kraken',), 'O': ('okx',), 'X': ('okx', 'coinbase', 'kraken')}
STRATEGIES = [('S0 сейчас', lambda s, sd: sim_s0(s, sd)),
              ('S2 итоговая', lambda s, sd: sim_sched(s, sd)),
              ('S3 WebSocket*', lambda s, sd: sim_ws(s, sd))]


def run(seeds=tuple(range(1, 9)), markdown=False):
    cols = ('период p50, с', 'p99/p50', 'CV', 'возраст p50, с', 'возраст p99, с', '>15 с, %', 'сборок/мин', 'запр/с', '429', 'восст., с')
    for scn in SCENARIOS:
        if markdown:
            print('\n**%s**\n' % scn.name)
            print('| Биржа | Стратегия | ' + ' | '.join(cols) + ' |')
            print('| --- | --- | ' + ' | '.join(['---'] * len(cols)) + ' |')
        else:
            print('\n=== %s' % scn.name)
        for venue in SHOW[scn.name[0]]:
            outage = (venue, scn.outage[venue][0], scn.outage[venue][1]) if venue in scn.outage else None
            for name, fn in STRATEGIES:
                acc = defaultdict(list)
                for sd in seeds:
                    r = fn(scn, sd).summarize(venue, outage)
                    for k, v in r.items():
                        acc[k].append(v)
                a = {k: st.mean(v) for k, v in acc.items()}
                f = lambda x, d=1: ('—' if x != x else ('%.' + str(d) + 'f') % x)
                row = [f(a['period']), f(a['ratio'], 2), f(a['cv'], 2), f(a['age50']), f(a['age99']), f(a['stale15']),
                       f(a['builds'], 0), f(a['reqs'], 2), f(a['n429']), f(a['rec'])]
                if markdown:
                    print('| %s | %s | ' % (venue, name) + ' | '.join(row) + ' |')
                else:
                    print('%-9s %-14s | ' % (venue, name) + '  '.join('%7s' % x for x in row))
    print('\n* S3 — справочно, допущения в sim_ws (не измерение). Возраст ограничен сверху 120 с. «восст.» — до первого свежего обновления после аварии.')


if __name__ == '__main__':
    import sys
    run(markdown='--markdown' in sys.argv)
