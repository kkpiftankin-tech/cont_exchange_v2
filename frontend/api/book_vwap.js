// Кумулятивный VWAP по уровням стакана (CALC-BOOK-VWAP). Единый источник формулы для
// BFF: график VectorClearing (buildSide в fetchVenueCurveImpl) и панель «Торговля»
// (/api/venues/orderbook). Только отображение/диагностика — в ledger не попадает,
// поэтому Number (double) допустим (CLAUDE.md §1.1).
//
// levels — уровни от ЛУЧШЕГО к худшему (ask по возрастанию, bid по убыванию цены).
// qtyKey — имя поля объёма уровня ("q" в роуте стакана, "v" в fetchVenueCurveImpl).
// Возвращает по точке на уровень: { p, qty, cum, notional, vwap }, где
//   cum      = Σ qty_j (j ≤ i),
//   notional = Σ p_j·qty_j,
//   vwap     = notional / cum.
// Порядок накопления фиксирован (слева направо) — результат побитово совпадает с прежним
// inline-кодом buildSide. Уровни с нефинитными/неположительными p или qty пропускаются.
function cumulativeBookVwap(levels, qtyKey = "q") {
  const out = [];
  let cum = 0, notional = 0;
  for (const lvl of Array.isArray(levels) ? levels : []) {
    const p = Number(lvl && lvl.p), qty = Number(lvl && lvl[qtyKey]);
    if (!Number.isFinite(p) || !Number.isFinite(qty) || p <= 0 || qty <= 0) continue;
    cum += qty;
    notional += p * qty;
    out.push({ p, qty, cum, notional, vwap: notional / cum });
  }
  return out;
}

module.exports = { cumulativeBookVwap };
