"""ВК = переводчики и связки-арбитражёры: клиринг на графе узлов «валюта@площадка».

Единицы: стоимость — тыс. USDT; лог-цены и отклонения — промилле (‰).
Произведение «тыс. USDT × ‰» = USDT.

УЗЛЫ.  BTC@{B,O,K}, USD@{B,O,K} — «валюта, лежащая на конкретной площадке».
       gBTC, gUSD — собственная книга биржи (её баланс по активу), потенциал задан
       внутренней маркой биржи μ (для нумерария USDT μ = 0).
       Потенциал узла x_u — стоимость единицы валюты этого узла, ‰ отклонения от эталона.
       Курс ребра σ_e = x_u − x_v.

РЁБРА. Каждое ребро — один виртуальный контрагент (ВК), то есть внутренний агент биржи:

  переводчик T_j — плечо котировки: (BTC@j → USD@j)
      якорь σ* = m_j (мид площадки j), глубина α_j (консервативный минорант книги ×
      хайркат θ), мёртвая зона c_j (комиссия + полспреда на площадке j).
      f > 0 = «биржа продала BTC на площадке j».

  переводчик T_j — плечо запаса: (BTC@j → gBTC) и (USD@j → gUSD)
      якорь σ* = −ρ·(Q_j − Q*) (сдвиг от собственного остатка на узле), глубина α_S,
      мёртвая зона = полоса безразличия. f > 0 = «остаток на узле j вырос».
      Σ_j f по этим рёбрам = изменение позиции биржи по активу за такт.

  связка-арбитражёр A_jk: (X@j → X@k) — физический перевод той же валюты
      якорь σ* = 0 (паритет), мёртвая зона c = тариф перевода, глубина α_A = W/(γσ²τ).
      f > 0 = «биржа перевела стоимость с площадки j на площадку k».

КРИВАЯ агента (одна и та же для всех типов):
    f_e(σ) = α_e · sign(σ*_e − σ_e) · max(0, |σ*_e − σ_e| − c_e)

КЛИРИНГ: баланс стоимости в каждом узле площадки. Узлы gBTC/gUSD — свободные
(их потенциал задан маркой), поэтому книга биржи может принимать дисбаланс.
Эквивалентно минимизации выпуклого потенциала
    Φ(x) = Σ_e ½ α_e ((|σ_e − σ*_e| − c_e)₊)²,   ∂Φ/∂x_u = −(нетто-исток из узла u).
"""
import numpy as np
from scipy.optimize import minimize

VEN = ["Binance", "OKX", "Kraken"]
SH = ["B", "O", "K"]
N = {}
for _i, _s in enumerate(SH):
    N[f"BTC@{_s}"] = _i
    N[f"USD@{_s}"] = 3 + _i
N["gBTC"], N["gUSD"] = 6, 7
NFREE, NNODE = 6, 8
PAIRS = ((0, 1), (0, 2), (1, 2))


class Graph:
    """Список рёбер в виде массивов. s — якоря, их можно менять между тактами."""

    def __init__(self, edges):
        self.name = [e[0] for e in edges]
        self.kind = [e[1] for e in edges]
        self.u = np.array([N[e[2]] for e in edges])
        self.v = np.array([N[e[3]] for e in edges])
        self.s = np.array([e[4] for e in edges], float)
        self.g = np.array([e[5] for e in edges], float)
        self.c = np.array([e[6] for e in edges], float)
        self.n = len(edges)
        self.is_quote = np.array([k == "котировка" for k in self.kind])
        self.is_tr = np.array([k == "перевод" for k in self.kind])
        self.is_st = np.array([k == "запас" for k in self.kind])
        self.btc = np.array([("BTC" in n) or ("_B_" in n) for n in self.name])


P0 = dict(aT=(60.0, 40.0, 25.0), cT=(0.10, 0.10, 0.14),   # переводчики: глубина и издержка
          aA_btc=12.0, cA_btc=0.25,                        # перевод BTC между площадками
          aA_usd=18.0, cA_usd=0.05,                        # перевод USDT между площадками
          aS_btc=8.0, cS_btc=0.15,                         # плечо запаса по BTC (полоса)
          aS_usd=60.0, cS_usd=0.02)                        # плечо запаса по USDT


def build(m, aT, cT, aA_btc, cA_btc, aA_usd, cA_usd, aS_btc, cS_btc, aS_usd, cS_usd):
    E = []
    for i, s in enumerate(SH):
        E.append((f"T_{s}", "котировка", f"BTC@{s}", f"USD@{s}", m[i], aT[i], cT[i]))
    for (i, j) in PAIRS:
        E.append((f"A_BTC_{SH[i]}{SH[j]}", "перевод", f"BTC@{SH[i]}", f"BTC@{SH[j]}", 0.0, aA_btc, cA_btc))
    for (i, j) in PAIRS:
        E.append((f"A_USD_{SH[i]}{SH[j]}", "перевод", f"USD@{SH[i]}", f"USD@{SH[j]}", 0.0, aA_usd, cA_usd))
    for s in SH:
        E.append((f"S_BTC_{s}", "запас", f"BTC@{s}", "gBTC", 0.0, aS_btc, cS_btc))
    for s in SH:
        E.append((f"S_USD_{s}", "запас", f"USD@{s}", "gUSD", 0.0, aS_usd, cS_usd))
    return Graph(E)


def flows(x, E):
    d = E.s - (x[E.u] - x[E.v])
    return E.g * np.sign(d) * np.maximum(0.0, np.abs(d) - E.c)


def _obj(xf, E, mu):
    x = np.concatenate([xf, [mu, 0.0]])
    d = E.s - (x[E.u] - x[E.v])
    dead = np.maximum(0.0, np.abs(d) - E.c)
    val = 0.5 * float(np.sum(E.g * dead ** 2))
    f = E.g * np.sign(d) * dead
    gr = np.zeros(NNODE)
    np.add.at(gr, E.u, -f)
    np.add.at(gr, E.v, +f)
    return val, gr[:NFREE]


def clear(E, mu, x0=None):
    x0 = np.zeros(NFREE) if x0 is None else np.asarray(x0[:NFREE], float)
    r = minimize(_obj, x0, args=(E, mu), jac=True, method="L-BFGS-B",
                 options=dict(maxiter=1000, ftol=1e-18, gtol=1e-13))
    x = np.concatenate([r.x, [mu, 0.0]])
    f = flows(x, E)
    imb = np.zeros(NNODE)
    np.add.at(imb, E.u, f)
    np.add.at(imb, E.v, -f)
    return dict(x=x, f=f, imb=imb[:NFREE], nit=r.nit)


def report(E, res):
    x, f = res["x"], res["f"]
    d = E.s - (x[E.u] - x[E.v])
    dead = np.maximum(0.0, np.abs(d) - E.c)
    is_sbtc = np.array([n.startswith("S_BTC") for n in E.name])
    return dict(
        gross=float(f @ d),                               # выигрыш по всему объёму, USDT
        quad=float(np.sum(f ** 2 / (2 * E.g))),           # проскальзывание / риск-стоимость
        fee=float(E.c @ np.abs(f)),                       # комиссии и тарифы
        pnl=float(f @ d) - float(np.sum(f ** 2 / (2 * E.g))) - float(E.c @ np.abs(f)),
        ident=0.5 * float(np.sum(E.g * dead ** 2)),       # закрытая форма того же
        dBTC=float(f[is_sbtc].sum()),                     # изменение позиции биржи по BTC
        sold=-float(f[E.is_quote].sum()),                 # нетто продано BTC переводчиками
        turn=float(np.abs(f).sum()),
        turn_q=float(np.abs(f[E.is_quote]).sum()),
        turn_a=float(np.abs(f[E.is_tr]).sum()),
        turn_s=float(np.abs(f[E.is_st]).sum()),
        d=d)


def mark(m, aT):
    """Внутренняя марка биржи: средневзвешенный по глубине мид площадок."""
    aT = np.asarray(aT, float)
    return float(np.asarray(m, float) @ aT / aT.sum())


# ===================================================================== проверки
if __name__ == "__main__":
    np.set_printoptions(precision=4, suppress=True)
    m = np.array([0.00, 0.35, -0.20])
    E = build(m, **P0)
    mu = mark(m, P0["aT"])
    res = clear(E, mu)
    rep = report(E, res)

    print("=== ОДИН ТАКТ =========================================================")
    print(f"миды площадок, ‰: {dict(zip(VEN, m))};   марка биржи μ = {mu:+.4f} ‰")
    print(f"итераций {res['nit']};  максимальный дисбаланс узла "
          f"{np.abs(res['imb']).max():.2e} тыс. USDT  (должен быть 0)")
    print("\nпотенциалы узлов, ‰:")
    print("   " + "  ".join(f"{k}={res['x'][i]:+.4f}" for k, i in N.items()))
    print("\nпотоки по агентам (тыс. USDT; знак — из первого узла во второй):")
    for i in range(E.n):
        tag = "   — мёртвая зона" if abs(res["f"][i]) < 1e-9 else ""
        print(f"   {E.name[i]:<12} {E.kind[i]:<10} α={E.g[i]:5.1f}  c={E.c[i]:.2f}  "
              f"перекос d={rep['d'][i]:+.4f}‰   f={res['f'][i]:+9.4f}{tag}")
    print(f"\nизлишек такта:  gross {rep['gross']:+.4f}  − риск/проскальзывание {rep['quad']:.4f}"
          f"  − тарифы {rep['fee']:.4f}  =  {rep['pnl']:+.5f} USDT")
    print(f"сверка с закрытой формой ½·Σ α(|d|−c)²₊ = {rep['ident']:+.5f} USDT")
    print(f"позиция биржи по BTC за такт: по плечам запаса {rep['dBTC']:+.5f}, "
          f"по котировкам {rep['dBTC']:+.5f} (продано переводчиками {rep['sold']:+.5f}) тыс. USDT")
    print(f"оборот: всего {rep['turn']:.3f} | котировки {rep['turn_q']:.3f} | "
          f"переводы {rep['turn_a']:.3f} | запас {rep['turn_s']:.3f}")

    print("\n=== ЧТО БУДЕТ, ЕСЛИ УБРАТЬ ТИПЫ АГЕНТОВ ==============================")
    for tag, over in (("полный набор", {}),
                      ("без связок-арбитражёров", dict(aA_btc=1e-9, aA_usd=1e-9)),
                      ("без плеч запаса", dict(aS_btc=1e-9, aS_usd=1e-9)),
                      ("без связок и без запаса", dict(aA_btc=1e-9, aA_usd=1e-9,
                                                       aS_btc=1e-9, aS_usd=1e-9))):
        PP = dict(P0); PP.update(over)
        Ei = build(m, **PP); ri = report(Ei, clear(Ei, mu))
        print(f"   {tag:<26} оборот котировок {ri['turn_q']:7.4f} тыс. USDT   "
              f"PnL {ri['pnl']:+.5f} USDT")
