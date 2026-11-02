// F-18: чистая (тестируемая) математика КОЛИЧЕСТВ по состоявшимся сделкам CE-агента (child_orders): сколько куплено/продано валюты и
// сколько исполнений. ЗАРАБОТОК (R, U, R+U) здесь НЕ считается: с ADR-087 п. 10 его единственный источник — ledger (GetAgentPnl);
// прежние формулы корзины сделок и круговой сделки (tradedBasketUsdt, pnlQuoteToUsdt, realized_quote, pnl_quote) удалены, чтобы
// расчёт не существовал в двух местах.
//
// raw: { buy_base, sell_base, buy_notional, sell_notional, fills } (все ≥0; base — base-актив, notional — валюта quote).
// Возвращает знаковые агрегаты:
//   traded_base  = buy_base − sell_base          (BUY +base, SELL −base)
//   traded_quote = sell_notional − buy_notional  (SELL +quote, BUY −quote)
function combineTradeStats(raw) {
  const n = (x) => (Number.isFinite(Number(x)) ? Number(x) : 0);
  return {
    traded_base: n(raw.buy_base) - n(raw.sell_base),
    traded_quote: n(raw.sell_notional) - n(raw.buy_notional),
    trade_fills: n(raw.fills),
  };
}

// Полная позиция агента (модель A7): c — неотправленный остаток, in_flight — уже отправленное в заявки; истинное
// обязательство = c + in_flight (обе величины одного знака), в тыс. USDT. Ровно так считает вкладка «Граф» (ce_graph_build.cOf),
// поэтому панель «Позиции» обязана брать ту же сумму, иначе ноги агента с заявкой «в полёте» на экранах расходятся.
// position не число ⇒ null (нет данных остаётся «нет данных», а не нулём); in_flight не число ⇒ 0.
function totalPosition(position, inFlight) {
  if (position === null || position === undefined) return null;
  const c = Number(position);
  if (!Number.isFinite(c)) return null;
  const f = Number(inFlight);
  return c + (Number.isFinite(f) ? f : 0);
}

// Стоимость позиции арбитражёра по ценам его бирж V = q_src·P_src + q_dst·P_dst раскладывается на две разные вещи:
//   открытая позиция (net) = (q_src + q_dst)·(P_src + P_dst)/2 — нетто-количество актива на двух ногах; у зеркальной позиции
//     (q_src = −q_dst) оно равно 0; растёт, когда ноги исполняются несимметрично (например, хедж одной ноги исполняется, другой
//     нет); это риск изменения цены, а не результат;
//   базис = (q_dst − q_src)/2·(P_dst − P_src) — разница цен между биржами на перевозимом объёме.
// Сумма частей равна V. Нумерарий (USDT) цены не имеет: обе части 0. Нет цены или количества ⇒ null.
function arbPositionParts(asset, numeraire, qSrc, pSrc, qDst, pDst) {
  if (asset && asset === numeraire) return { open_position_usdt: 0, basis_usdt: 0 };
  if (qSrc === null || qSrc === undefined || qDst === null || qDst === undefined) return null;
  const a = Number(qSrc), b = Number(qDst), ps = Number(pSrc), pd = Number(pDst);
  if (!Number.isFinite(a) || !Number.isFinite(b) || !(ps > 0) || !(pd > 0)) return null;
  return {
    open_position_usdt: (a + b) * (ps + pd) / 2,
    basis_usdt: ((b - a) / 2) * (pd - ps),
  };
}

// Статистика сделок с разбивкой по биржам: rows — строки SQL, сгруппированные по (agent_id, venue), с теми же беззнаковыми суммами,
// что принимает combineTradeStats. Итог по агенту — combineTradeStats от СУММЫ сырых значений по всем биржам; by_venue[venue] — количества сделок именно на этой бирже (панель показывает их в строке ноги
// арбитражёра: сколько базовой и котируемой валюты сделано на источнике и на приёмнике). Map(agent_id → статистика + by_venue).
const RAW_KEYS = ['buy_base', 'sell_base', 'buy_notional', 'sell_notional', 'fills'];
function combineTradeStatsByVenue(rows) {
  const n = (x) => (Number.isFinite(Number(x)) ? Number(x) : 0);
  const sums = new Map();      // agent_id → суммы по всем биржам
  const perVenue = new Map();  // agent_id → Map(venue → сырые суммы)
  for (const r of Array.isArray(rows) ? rows : []) {
    if (!r || !r.agent_id) continue;
    const venue = r.venue || '';
    if (!sums.has(r.agent_id)) { sums.set(r.agent_id, {}); perVenue.set(r.agent_id, new Map()); }
    const tot = sums.get(r.agent_id);
    const pv = perVenue.get(r.agent_id);
    if (!pv.has(venue)) pv.set(venue, {});
    const cell = pv.get(venue);
    for (const k of RAW_KEYS) {
      tot[k] = n(tot[k]) + n(r[k]);
      cell[k] = n(cell[k]) + n(r[k]);
    }
  }
  const out = new Map();
  for (const [agentId, tot] of sums) {
    const by = {};
    for (const [venue, cell] of perVenue.get(agentId)) {
      const c = combineTradeStats(cell);
      by[venue] = { traded_base: c.traded_base, traded_quote: c.traded_quote, trade_fills: c.trade_fills };
    }
    out.set(agentId, { ...combineTradeStats(tot), by_venue: by });
  }
  return out;
}

module.exports = { combineTradeStats, combineTradeStatsByVenue, totalPosition, arbPositionParts };
