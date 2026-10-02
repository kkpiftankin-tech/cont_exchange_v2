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
  return {
    traded_base: n(raw.buy_base) - n(raw.sell_base),
    traded_quote: n(raw.sell_notional) - n(raw.buy_notional),
    pnl_quote: n(raw.sell_pnlbase) - n(raw.buy_pnlbase),
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

module.exports = { combineTradeStats, pnlQuoteToUsdt };
