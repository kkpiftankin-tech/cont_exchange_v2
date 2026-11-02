"""Прогоны и рисунки к дизайну «ВК = переводчики и связки-арбитражёры».

A. Порог включения контура по разрыву цен между площадками.
B. Полоса безразличия плеча запаса: арбитраж против позиционирования.
C. Смещение собственной марки биржи → систематический дрейф позиции.
D. 400 тактов, три политики: решение по агрегату / по агентам / по агентам + лимит.
"""
import os
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch

from agents_vc import (VEN, SH, N, PAIRS, P0, build, clear, report, mark, NFREE)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "img")
os.makedirs(OUT, exist_ok=True)
INK = "#1f1f1f"; GRAY = "#8a8a8a"; GRID = "#e6e6e6"
EM = "#1B7F5E"; BR = "#B5472E"; BLU = "#2D5F8B"; SAND = "#C99A2E"
plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 10,
    "axes.edgecolor": GRAY, "axes.labelcolor": INK, "xtick.color": INK, "ytick.color": INK,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False,
    "figure.dpi": 150, "savefig.dpi": 150, "savefig.bbox": "tight",
})
def ttl(ax, t): ax.set_title(t, loc="left", fontsize=10.5, color=INK, pad=9)


def run_once(m, over=None, mu=None):
    PP = dict(P0)
    if over:
        PP.update(over)
    E = build(m, **PP)
    mu = mark(m, PP["aT"]) if mu is None else mu
    r = clear(E, mu)
    return E, r, report(E, r)


# =============================================== A. порог по разрыву площадок
eps = np.linspace(0.0, 2.0, 501)
A = {k: [] for k in ("pnl", "tq", "ta", "ts", "dpos")}
for e in eps:
    m = np.array([0.0, +e / 2, -e / 2])
    _, _, r = run_once(m)
    A["pnl"].append(r["pnl"]); A["tq"].append(r["turn_q"])
    A["ta"].append(r["turn_a"]); A["ts"].append(r["turn_s"])
    A["dpos"].append(abs(r["dBTC"]))
A = {k: np.array(v) for k, v in A.items()}

# теоретические пороги: сумма мёртвых зон по маршруту
thr_arb = P0["cT"][1] + P0["cT"][2] + P0["cA_btc"] + min(P0["cA_usd"], 2 * P0["cS_usd"])
mu_coef = (P0["aT"][1] - P0["aT"][2]) / 2 / sum(P0["aT"])       # μ = mu_coef·ε
thr_posK = (P0["cS_btc"] + P0["cT"][2] + P0["cS_usd"]) / (0.5 + mu_coef)
thr_posO = (P0["cS_btc"] + P0["cT"][1] + P0["cS_usd"]) / (0.5 - mu_coef)
first_trade = eps[np.argmax(A["tq"] > 1e-6)]
first_pos = eps[np.argmax(A["dpos"] > 1e-6)]
print("=== A. Порог включения контура ==========================================")
print(f"  расчётный порог чистого арбитража (цикл OKX↔Kraken): {thr_arb:.3f} ‰")
print(f"  расчётный порог позиционирования у Kraken / OKX:     {thr_posK:.3f} / {thr_posO:.3f} ‰")
print(f"  наблюдённый порог первой сделки:                     {first_trade:.3f} ‰")
print(f"  наблюдённый порог первого изменения позиции:         {first_pos:.3f} ‰")

# =============================================== B. полоса плеча запаса
bands = np.linspace(0.01, 0.80, 160)
B = {k: [] for k in ("pnl", "tq", "ts", "dpos")}
m_fix = np.array([0.0, +0.60, -0.60])
for b in bands:
    _, _, r = run_once(m_fix, over=dict(cS_btc=b))
    B["pnl"].append(r["pnl"]); B["tq"].append(r["turn_q"])
    B["ts"].append(r["turn_s"]); B["dpos"].append(abs(r["dBTC"]))
B = {k: np.array(v) for k, v in B.items()}
pnl_arb = B["pnl"][-1]                       # предел при широкой полосе = чистый арбитраж
print("\n=== B. Полоса плеча запаса (разрыв 1.2 ‰) ==============================")
for b in (0.02, 0.15, 0.30, 0.60):
    j = int(np.argmin(np.abs(bands - b)))
    print(f"  полоса {bands[j]:.2f} ‰: оборот котировок {B['tq'][j]:6.3f}, оборот плеч запаса "
          f"{B['ts'][j]:6.3f}, |Δпозиции| {B['dpos'][j]:6.3f} тыс. USDT, PnL {B['pnl'][j]:+.4f} USDT")
print(f"  предел широкой полосы (чистый арбитраж): PnL {pnl_arb:+.4f} USDT; "
      f"доля заработка от позиции при полосе 0.02 ‰ = "
      f"{100 * (B['pnl'][0] - pnl_arb) / B['pnl'][0]:.0f} %")

# =============================================== C. смещение собственной марки
bias = np.linspace(-0.5, 0.5, 101)
m_c = np.array([0.0, +0.60, -0.60])
C = np.array([run_once(m_c, mu=mark(m_c, P0["aT"]) + b)[2]["dBTC"] for b in bias])
print("\n=== C. Смещение марки биржи ============================================")
for b in (-0.3, 0.0, 0.3):
    j = int(np.argmin(np.abs(bias - b)))
    print(f"  смещение марки {bias[j]:+.2f} ‰ → позиция за такт {C[j]:+.4f} тыс. USDT")

# =============================================== D. 400 тактов, три политики
RNG = np.random.default_rng(7)
T = 400
mids = np.zeros((T, 3))
for t in range(1, T):
    mids[t] = 0.88 * mids[t - 1] + 0.30 * RNG.standard_normal(3)

QSTAR, QUSTAR = 40.0, 40.0          # целевой запас на площадке, тыс. USDT
RHO_N, RHO_A = 0.004, 0.004          # сдвиг якоря на тыс. отклонения запаса, ‰
ZLIM, RHO_L = 60.0, 0.02             # агрегатный лимит и жёсткость овеpрайда
LAT = 5                              # такты доставки физического перевода
PEN = 1.50                           # ‰ штраф за срочное закрытие нехватки на узле

IDX_T = [0, 1, 2]
IDX_ABTC = [3, 4, 5]
IDX_AUSD = [6, 7, 8]
IDX_SBTC = [9, 10, 11]
IDX_SUSD = [12, 13, 14]


def simulate(policy, pen=PEN):
    E = build(mids[0], **P0)
    Q = np.full(3, QSTAR); QU = np.full(3, QUSTAR)
    transit = []                                   # (t_arrive, asset, venue, amount)
    Z = 0.0; ZU = 0.0
    x0 = np.zeros(NFREE)
    hist = dict(pnl=np.zeros(T), Z=np.zeros(T), Q=np.zeros((T, 3)),
                short=np.zeros(T), tr=np.zeros(T), turn=np.zeros(T))
    cum = 0.0; mu_prev = 0.0; n_short = 0; n_tr = 0
    for t in range(T):
        m = mids[t]
        mu = mark(m, P0["aT"])
        E.s[IDX_T] = m
        if policy == "agg":
            E.s[IDX_SBTC] = -RHO_A * Z
            E.s[IDX_SUSD] = -RHO_A * ZU
        else:
            E.s[IDX_SBTC] = -RHO_N * (Q - QSTAR)
            E.s[IDX_SUSD] = -RHO_N * (QU - QUSTAR)
            if policy == "gate" and abs(Z) > ZLIM:
                E.s[IDX_SBTC] -= RHO_L * (Z - np.sign(Z) * ZLIM)
        res = clear(E, mu, x0); x0 = res["x"][:NFREE]
        rep = report(E, res); f = res["f"]

        # физические переводы: уходят сразу, приходят через LAT тактов
        out_b = np.zeros(3); out_u = np.zeros(3)
        for k, (i, j) in enumerate(PAIRS):
            fb = f[IDX_ABTC[k]]
            if fb > 0: out_b[i] += fb; transit.append((t + LAT, "B", j, fb))
            elif fb < 0: out_b[j] += -fb; transit.append((t + LAT, "B", i, -fb))
            fu = f[IDX_AUSD[k]]
            if fu > 0: out_u[i] += fu; transit.append((t + LAT, "U", j, fu))
            elif fu < 0: out_u[j] += -fu; transit.append((t + LAT, "U", i, -fu))
        n_tr += int(np.abs(f[IDX_ABTC]).sum() > 1e-9)

        sold = f[IDX_T]                                   # + продали BTC на площадке
        dQ = -sold - out_b
        dQU = +sold - out_u
        arrivals = [a for a in transit if a[0] == t]
        transit = [a for a in transit if a[0] != t]
        for _, asset, ven, amt in arrivals:
            if asset == "B": dQ[ven] += amt
            else: dQU[ven] += amt

        short = float(np.sum(np.maximum(0.0, -(Q + dQ))) + np.sum(np.maximum(0.0, -(QU + dQU))))
        n_short += int(short > 1e-9)
        Q = np.maximum(Q + dQ, 0.0); QU = np.maximum(QU + dQU, 0.0)
        Z = float(Q.sum() + sum(a[3] for a in transit if a[1] == "B") - 3 * QSTAR)
        ZU = float(QU.sum() + sum(a[3] for a in transit if a[1] == "U") - 3 * QUSTAR)

        cum += rep["pnl"] + Z * (mu - mu_prev) - pen * short
        mu_prev = mu
        hist["pnl"][t] = cum; hist["Z"][t] = Z; hist["Q"][t] = Q
        hist["short"][t] = short; hist["turn"][t] = rep["turn_q"]
    hist["n_short"] = n_short; hist["n_tr"] = n_tr
    return hist


POL = {"agg": "решение по агрегату биржи", "node": "по агентам, без агрегатного лимита",
       "gate": "по агентам + агрегатный лимит"}
S = {k: simulate(k) for k in POL}
print("\n=== D. 400 тактов ======================================================")
print(f"{'политика':<36} {'PnL, USDT':>11} {'оборот':>10} {'max|Z|':>9} "
      f"{'тактов с нехваткой':>19} {'переводов':>10}")
for k, lab in POL.items():
    h = S[k]
    print(f"{lab:<36} {h['pnl'][-1]:>11.1f} {h['turn'].sum():>10.1f} "
          f"{np.abs(h['Z']).max():>9.1f} {h['n_short']:>19d} {h['n_tr']:>10d}")
print("\n  чувствительность к цене срочного закрытия нехватки на узле:")
for pen in (0.6, 1.5, 3.0):
    row = {k: simulate(k, pen)["pnl"][-1] for k in POL}
    print(f"    штраф {pen:.1f} ‰:  агрегат {row['agg']:8.1f} | по агентам {row['node']:8.1f} "
          f"| агенты+лимит {row['gate']:8.1f} USDT")

# =============================================== рисунок 11: граф и пороги
fig = plt.figure(figsize=(13.4, 4.7), constrained_layout=True)
gs = fig.add_gridspec(2, 3, width_ratios=[1.20, 0.92, 1.02], height_ratios=[1.35, 1.0])

# --- панель A: граф с потоками одного такта
ax = fig.add_subplot(gs[:, 0]); ax.set_axis_off()
m1 = np.array([0.00, 0.35, -0.20]); E1, r1, rep1 = run_once(m1)
pos = {"BTC@B": (0, 2.0), "BTC@O": (2, 2.0), "BTC@K": (4, 2.0),
       "USD@B": (0, 0.55), "USD@O": (2, 0.55), "USD@K": (4, 0.55)}
inv = {v: k for k, v in N.items()}
LW = lambda f: 1.2 + 14 * abs(f)


def arrow(a, b, col, f, rad=0.0, shr=15):
    s, e = (a, b) if f > 0 else (b, a)
    ax.add_patch(FancyArrowPatch(s, e, connectionstyle=f"arc3,rad={rad}",
                                 arrowstyle="-|>", mutation_scale=12, lw=LW(f),
                                 color=col, zorder=3, shrinkA=shr, shrinkB=shr, alpha=0.92))


for i in range(E1.n):
    fv = r1["f"][i]; kind = E1.kind[i]
    nu, nv = inv[E1.u[i]], inv[E1.v[i]]
    if kind == "запас":                                     # стрелка-«культя» к книге биржи
        px, py = pos[nu]
        off = 1.00 if nu.startswith("BTC") else -1.00
        if abs(fv) < 1e-6:
            ax.plot([px, px], [py + 0.32 * np.sign(off), py + off], color=GRID, lw=1, zorder=1)
            continue
        a, b = (px, py + off), (px, py + 0.30 * np.sign(off))
        arrow(a, b, EM, fv, shr=4)                           # f>0 — запас узла растёт
        ax.text(px + 0.16, py + off * 0.72, f"{abs(fv):.3f}", fontsize=7.0, color=EM)
        continue
    a, b = pos[nu], pos[nv]
    if abs(fv) < 1e-6:
        rad = 0.30 if (kind == "перевод" and abs(a[0] - b[0]) > 2.5) else 0.0
        ax.add_patch(FancyArrowPatch(a, b, connectionstyle=f"arc3,rad={rad}", arrowstyle="-",
                                     color=GRID, lw=1.1, zorder=1, shrinkA=15, shrinkB=15))
        continue
    col = BR if kind == "котировка" else BLU
    rad = -0.30 if (kind == "перевод" and nu.startswith("BTC")) else (
        0.30 if kind == "перевод" else 0.0)
    arrow(a, b, col, fv, rad)
    xm, ym = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
    ax.text(xm + (0.30 if kind == "котировка" else 0), ym + (0 if kind == "котировка" else -0.34),
            f"{abs(fv):.3f}", fontsize=7.0, color=col, ha="center")
for k, (px, py) in pos.items():
    ax.scatter([px], [py], s=720, facecolor="white", edgecolor=INK, lw=1.2, zorder=4)
    ax.text(px, py, k.replace("BTC@", "BTC\n").replace("USD@", "USDT\n"),
            ha="center", va="center", fontsize=7.2, color=INK, zorder=5)
ax.text(2.0, 3.28, "собственная книга биржи по BTC", fontsize=7.4, color=EM, ha="center")
ax.text(2.0, -0.92, "собственная книга биржи по USDT", fontsize=7.4, color=EM, ha="center")
ax.text(-1.0, 3.92, "миды: Binance 0,00 · OKX +0,35 · Kraken −0,20 ‰;   "
        f"марка биржи μ = {mark(m1, P0['aT']):+.3f} ‰", fontsize=7.6, color=GRAY)
ax.text(-1.0, 3.63, f"Σ по плечам запаса BTC = {rep1['dBTC']:+.3f} тыс. USDT — "
        "это и есть позиция биржи за такт", fontsize=7.6, color=EM)
for lab, col, y in (("переводчик: котировка книги площадки", BR, -1.42),
                    ("связка-арбитражёр: физический перевод", BLU, -1.75),
                    ("переводчик: плечо запаса (стрелка к узлу — запас растёт)", EM, -2.08)):
    ax.plot([-0.75, -0.30], [y, y], color=col, lw=2.5)
    ax.text(-0.15, y, lab, fontsize=7.4, color=INK, va="center")
ax.set_xlim(-1.1, 5.0); ax.set_ylim(-2.35, 4.05)
ttl(ax, "Один такт: продали BTC на OKX, купили на Kraken,\nмонеты поехали K→O, "
        "разницу приняла книга биржи")

# --- панель B: кривые двух типов агентов
ax = fig.add_subplot(gs[:, 1])
g = np.linspace(-0.62, 0.62, 400)
for lab, a_, c_, col, ls in (("переводчик OKX: α=40, c=0,10 ‰", 40, 0.10, BR, "-"),
                             ("связка BTC O↔K: α=12, c=0,25 ‰", 12, 0.25, BLU, "--"),
                             ("плечо запаса BTC: α=8, c=0,15 ‰", 8, 0.15, EM, ":")):
    ax.plot(g, a_ * np.sign(g) * np.maximum(0, np.abs(g) - c_), color=col, lw=2, ls=ls, label=lab)
ax.axhline(0, color=GRAY, lw=1); ax.axvline(0, color=GRAY, lw=1)
ax.axvspan(-0.25, 0.25, color=BLU, alpha=0.06, lw=0)
ax.annotate("мёртвая зона связки шире:\nфизический перевод дороже\nсделки в книге",
            xy=(0.25, 0), xytext=(0.05, -17), fontsize=7.4, color=INK,
            arrowprops=dict(arrowstyle="->", color=GRAY))
ax.set_xlabel("перекос якоря к курсу клиринга d = σ* − σ, ‰")
ax.set_ylabel("поток агента f, тыс. USDT")
ttl(ax, "Кривая у всех одна,\nразличаются глубина и мёртвая зона")
ax.legend(loc="upper left", fontsize=7.4, frameon=False)

# --- панель C: порог по разрыву (два яруса, общий x)
axC = fig.add_subplot(gs[0, 2])
axC.plot(eps, A["tq"], color=BR, lw=2, label="оборот котировок")
axC.plot(eps, A["ta"], color=BLU, lw=2, ls="--", label="оборот физических переводов")
axC.axvline(first_trade, color=BLU, lw=1, ls="-.")
axC.set_ylabel("оборот, тыс. USDT/такт")
axC.set_xticklabels([])
axC.text(first_trade + 0.04, A["tq"].max() * 0.60,
         f"порог арбитража {first_trade:.2f} ‰\n= сумма мёртвых зон маршрута", fontsize=7.2, color=BLU)
ttl(axC, "Контур молчит, пока разрыв меньше издержек маршрута")
axC.legend(loc="upper left", fontsize=7.4, frameon=False)

axD = fig.add_subplot(gs[1, 2], sharex=axC)
axD.plot(eps, A["dpos"], color=EM, lw=2)
axD.axvline(first_trade, color=BLU, lw=1, ls="-.")
axD.axvline(first_pos, color=EM, lw=1, ls="-.")
axD.text(first_pos + 0.06, A["dpos"].max() * 0.22,
         f"позиция появляется позже,\nс {first_pos:.2f} ‰", fontsize=7.2, color=EM)
axD.set_xlabel("разрыв цен OKX − Kraken, ‰")
axD.set_ylabel("|Δпозиции|, тыс. USDT")
fig.savefig(os.path.join(OUT, "fig11_agents_graph.png")); plt.close(fig)

# =============================================== рисунок 12: полоса и марка
fig, axes = plt.subplots(1, 3, figsize=(13.4, 4.2), constrained_layout=True)

ax = axes[0]
ax.plot(bands, B["pnl"], color=INK, lw=2, label="излишек такта, всего")
ax.axhline(pnl_arb, color=BLU, lw=1.4, ls="--")
ax.fill_between(bands, pnl_arb, B["pnl"], color=EM, alpha=0.14)
ax.fill_between(bands, 0, pnl_arb, color=BLU, alpha=0.10)
ax.axvline(P0["cS_btc"], color=GRAY, lw=1, ls=":")
ax.text(P0["cS_btc"] + 0.014, B["pnl"].max() * 1.05, "полоса базового набора 0,15 ‰",
        fontsize=7.4, color=GRAY)
ax.text(0.38, pnl_arb * 0.42, f"чистый арбитраж:\nбез позиции, {pnl_arb:.2f} USDT",
        fontsize=7.6, color=BLU)
ax.text(0.21, (B["pnl"][0] + pnl_arb) / 2 * 0.99, "надбавка\nза принятую позицию",
        fontsize=7.6, color=EM)
ax.set_ylim(0, B["pnl"].max() * 1.20)
ax.set_xlabel("полоса безразличия плеча запаса c_S, ‰"); ax.set_ylabel("USDT за такт")
ttl(ax, "Полоса запаса — ручка «арбитраж или позиция»\n(разрыв площадок 1,2 ‰)")
ax.legend(loc="lower right", fontsize=7.8, frameon=False)

ax = axes[1]
ax.plot(bands, B["ts"], color=EM, lw=2, label="оборот плеч запаса Σ|f_S|")
ax.plot(bands, B["dpos"], color=BR, lw=2, ls="--", label="|изменение позиции биржи|")
ax.plot(bands, B["tq"], color=GRAY, lw=1.4, ls=":", label="оборот котировок")
ax.axvline(P0["cS_btc"], color=GRAY, lw=1, ls=":")
ax.annotate("нетто-позиция мала, пока обе площадки\nработают навстречу друг другу",
            xy=(0.10, B["dpos"][int(np.argmin(np.abs(bands - 0.10)))]),
            xytext=(0.26, B["ts"].max() * 0.30), fontsize=7.4, color=BR,
            arrowprops=dict(arrowstyle="->", color=GRAY))
ax.set_xlabel("полоса безразличия плеча запаса c_S, ‰"); ax.set_ylabel("тыс. USDT за такт")
ttl(ax, "Оборот собственной книги гаснет с полосой,\nа нетто-позиция ведёт себя немонотонно")
ax.legend(loc="upper right", fontsize=7.8, frameon=False)

ax = axes[2]
ax.plot(bias, C, color=INK, lw=2)
ax.axhline(0, color=GRAY, lw=1); ax.axvline(0, color=GRAY, lw=1)
ax.fill_between(bias, C, 0, where=C > 0, color=EM, alpha=0.13)
ax.fill_between(bias, C, 0, where=C < 0, color=BR, alpha=0.13)
ax.annotate("марка завышена → биржа\nкаждый такт докупает актив",
            xy=(0.35, C[int(np.argmin(np.abs(bias - 0.35)))]), xytext=(-0.10, C.max() * 0.72),
            fontsize=7.8, color=EM, arrowprops=dict(arrowstyle="->", color=GRAY))
ax.annotate("марка занижена → биржа\nкаждый такт распродаёт",
            xy=(-0.35, C[int(np.argmin(np.abs(bias + 0.35)))]), xytext=(-0.48, C.min() * 0.55),
            fontsize=7.8, color=BR, arrowprops=dict(arrowstyle="->", color=GRAY))
ax.set_xlabel("смещение собственной марки биржи μ от консенсуса, ‰")
ax.set_ylabel("изменение позиции за такт, тыс. USDT")
ttl(ax, "Дрейф позиции — индикатор ошибки собственной марки,\nа не свойство клиринга")
fig.savefig(os.path.join(OUT, "fig12_band_and_mark.png")); plt.close(fig)

# =============================================== рисунок 13: три политики
fig, axes = plt.subplots(1, 3, figsize=(13.4, 4.2), constrained_layout=True)
cols = {"agg": INK, "node": BR, "gate": EM}
lss = {"agg": ":", "node": "--", "gate": "-"}
ax = axes[0]
for k, lab in POL.items():
    ax.plot(S[k]["pnl"], color=cols[k], lw=2, ls=lss[k], label=lab)
ax.axhline(0, color=GRAY, lw=1)
ax.set_xlabel("такт"); ax.set_ylabel("накопленный PnL, USDT")
ttl(ax, f"Накопленный результат за 400 тактов\n(срочное закрытие нехватки на узле — {PEN:.1f} ‰)")
ax.legend(loc="upper left", fontsize=7.8, frameon=False)

ax = axes[1]
for k, lab in POL.items():
    ax.plot(S[k]["Z"], color=cols[k], lw=1.6, ls=lss[k], label=lab)
ax.axhline(ZLIM, color=EM, lw=1, ls="-."); ax.axhline(-ZLIM, color=EM, lw=1, ls="-.")
ax.axhline(0, color=GRAY, lw=1)
zmax = max(np.abs(S[k]["Z"]).max() for k in POL)
ax.set_ylim(-ZLIM * 1.6, zmax * 1.14)
ax.text(10, -ZLIM + zmax * 0.03, f"агрегатный лимит ±{ZLIM:.0f} тыс. USDT",
        fontsize=7.6, color=EM)
ax.set_xlabel("такт"); ax.set_ylabel("позиция биржи по BTC, тыс. USDT")
ttl(ax, "Агрегат — это риск: он остаётся в лимите,\nтолько если под контролем узлы")

ax = axes[2]
for i in range(3):
    ax.plot(S["agg"]["Q"][:, i], color=INK, lw=1.0, alpha=0.30 + 0.15 * i,
            label="по агрегату (три площадки)" if i == 0 else None)
    ax.plot(S["gate"]["Q"][:, i], color=EM, lw=1.4, alpha=0.45 + 0.18 * i,
            label="по агентам (три площадки)" if i == 0 else None)
ax.axhline(QSTAR, color=GRAY, lw=1, ls=":")
ax.axhline(0, color=BR, lw=1.2)
ax.set_ylim(-18, max(S["agg"]["Q"].max(), S["gate"]["Q"].max()) * 1.10)
ax.text(396, -12, f"ноль запаса: нога неисполнима "
        f"({S['agg']['n_short']} тактов против {S['gate']['n_short']})",
        fontsize=7.4, color=BR, ha="right")
ax.text(396, QSTAR * 5.6, "целевой запас 40 тыс. USDT", fontsize=7.4, color=GRAY, ha="right")
ax.set_xlabel("такт"); ax.set_ylabel("запас BTC на площадке, тыс. USDT")
ttl(ax, "Узловой запас — это исполнимость:\nагрегат её не видит и площадки высыхают")
ax.legend(loc="upper left", fontsize=7.6, frameon=False)
fig.savefig(os.path.join(OUT, "fig13_policies.png")); plt.close(fig)
print("\nрисунки: fig11_agents_graph.png, fig12_band_and_mark.png, fig13_policies.png")
