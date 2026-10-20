// F-18: чистая (тестируемая) математика статистики по состоявшимся сделкам CE-агента.
// Заземление: docs/04-domain/calculations/CALC-F12-HEDGE-PNL.md (hedgePnL=s·(p_exec−p_int)·q,
// s=+1 SELL / −1 BUY; комиссии НЕ входят). Знаковую комбинацию держим ЗДЕСЬ (а не в SQL):
// SQL отдаёт БЕЗЗНАКОВЫЕ per-side суммы (однозначно), знак применяется в JS и покрыт тестом
// (trade_stats_math.test.js), т.к. именно sign-flip BUY/SELL — место тихой ошибки.

// raw: { buy_base, sell_base, buy_notional, sell_notional, buy_pnlbase, sell_pnlbase, fills }
// (все ≥0, в единицах: base — base-актив; notional/pnlbase — валюта quote).
// Возвращает знаковые агрегаты по сделкам:
//   traded_base  = Σ(−s)·q           = buy_base − sell_base        (BUY +base, SELL −base)
//   traded_quote = Σ s·q·p_exec      = sell_notional − buy_notional (SELL +quote, BUY −quote)
//   pnl_quote    = Σ s·(p_exec−p_int)·q = sell_pnlbase − buy_pnlbase (в валюте quote)
function combineTradeStats(raw) {
  const n = (x) => (Number.isFinite(Number(x)) ? Number(x) : 0);
  // ADR-072 §C / CALC-CE-TRANSLATOR-PNL: realized P&L на ЗАКРЫТОМ объёме round-trip
  // = min(B,S)·(avg_pS − avg_pB), в валюте quote. B/S — объёмы buy/sell (base),
  // avg_pB/avg_pS — средние цены. Нарастает только при закрытии (unrealized — остаток,
  // считается в BFF как (R+U) − R). Нужно, чтобы заработок был ВИДЕН отдельно от MTM.
  const B = n(raw.buy_base), S = n(raw.sell_base);
  const avgB = B > 0 ? n(raw.buy_notional) / B : 0;
  const avgS = S > 0 ? n(raw.sell_notional) / S : 0;
  const realized_quote = Math.min(B, S) * (avgS - avgB);
  return {
    traded_base: n(raw.buy_base) - n(raw.sell_base),
    traded_quote: n(raw.sell_notional) - n(raw.buy_notional),
    pnl_quote: n(raw.sell_pnlbase) - n(raw.buy_pnlbase),
    realized_quote,
    trade_fills: n(raw.fills),
  };
}

// Перевод заработка из валюты quote в USDT. P(quote)_USDT = base_price/price, где price —
// цена пары (base/quote, конвенция clearing-lambda). */USDT и арбитражёр: price==base_price
// ⇒ ×1. Кросс (ETH/BTC): base_price/price = P(ETH)/(P(ETH)/P(BTC)) = P(BTC)_USDT. Нет цены
// ⇒ ×1 (не искажаем знак/масштаб обнулением; редкий edge без position-строки).
function pnlQuoteToUsdt(pnlQuote, basePrice, price) {
  const p = Number(price), bp = Number(basePrice), e = Number(pnlQuote);
  if (!Number.isFinite(e)) return null;
  const qrate = (p > 0 && bp > 0) ? (bp / p) : 1;
  return e * qrate;
}

// Заработок = mark-to-market КОРЗИНЫ состоявшихся сделок по ТЕКУЩИМ клиринговым ценам
// (правило владельца 2026-10-05): купили одну валюту, продали другую — оценили обе ноги
// в USDT «сейчас». net_base=traded_base (BUY +base, SELL −base), net_quote=traded_quote
// (SELL +quote, BUY −quote). P(base)_USDT=basePrice, P(quote)_USDT=basePrice/price
// (price=цена пары base/quote; */USDT ⇒ price==basePrice ⇒ qrate=1). Включает и
// реализованный кросс-спред арбитражёра (net_base≈0, net_quote=q·(P_dst−P_src)), и
// нереализованный P&L инвентаря переводчика (меняется с маркой — отсюда «рост»).
// Комиссии НЕ входят (owner: пока не учитываем). Может быть отрицательным.
function tradedBasketUsdt(tradedBase, tradedQuote, basePrice, price) {
  const nb = Number(tradedBase), nq = Number(tradedQuote);
  const bp = Number(basePrice), pr = Number(price);
  if (!Number.isFinite(nb) && !Number.isFinite(nq)) return null;
  const baseLeg = (Number.isFinite(nb) && bp > 0) ? nb * bp : 0;     // net_base·P(base)_USDT
  const qrate = (pr > 0 && bp > 0) ? (bp / pr) : 1;                  // P(quote)_USDT
  const quoteLeg = Number.isFinite(nq) ? nq * qrate : 0;            // net_quote·P(quote)_USDT
  return baseLeg + quoteLeg;
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

// Базис позиции арбитражёра: стоимость обеих его ног (в единицах актива, ноги src и dst противоположны по знаку)
// по ценам СВОИХ бирж = q_src·P_src + q_dst·P_dst = q_dst·(P_dst − P_src). Это результат «закрыть позицию сейчас по ценам
// бирж» при нулевой стоимости перевода; он нереализован — арбитражёр наружу не торгует (ADR-073). Нумерарий (USDT) цены
// не имеет и базиса не даёт ⇒ 0. Нет цены или количества ⇒ null.
function arbBasisMtmUsdt(asset, numeraire, qSrc, pSrc, qDst, pDst) {
  if (asset && asset === numeraire) return 0;
  const a = Number(qSrc), b = Number(qDst), ps = Number(pSrc), pd = Number(pDst);
  if (qSrc === null || qSrc === undefined || qDst === null || qDst === undefined) return null;
  if (!Number.isFinite(a) || !Number.isFinite(b) || !(ps > 0) || !(pd > 0)) return null;
  return a * ps + b * pd;
}

module.exports = { combineTradeStats, pnlQuoteToUsdt, tradedBasketUsdt, totalPosition, arbBasisMtmUsdt };
