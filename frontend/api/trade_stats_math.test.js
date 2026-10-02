// Self-contained node-тест (у frontend/api нет jest): `node trade_stats_math.test.js`.
// Проверяет знаки base/quote/pnl (CALC-F12-HEDGE-PNL) и перевод pnl в USDT.
const assert = require('assert');
const { combineTradeStats, pnlQuoteToUsdt } = require('./trade_stats_math');

let failed = 0;
const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}

// Числовой пример (trading-domain-specialist, T_ETH ETH/USDT):
//   Fill1 SELL: p_int=2500, p_exec=2510, q=0.4
//   Fill2 BUY:  p_int=2480, p_exec=2470, q=0.2
// per-side безнаковые суммы (как из SQL):
const raw = {
  buy_base: 0.2, sell_base: 0.4,
  buy_notional: 0.2 * 2470, sell_notional: 0.4 * 2510,          // 494 / 1004
  buy_pnlbase: (2470 - 2480) * 0.2, sell_pnlbase: (2510 - 2500) * 0.4, // -2 / +4
  fills: 2,
};
const s = combineTradeStats(raw);
// Δbase = 0.2 − 0.4 = −0.2; Δquote = 1004 − 494 = +510; pnl = 4 − (−2) = +6
check('base = -0.2 (BUY +, SELL -)', near(s.traded_base, -0.2));
check('quote = +510 (SELL +, BUY -)', near(s.traded_quote, 510));
check('pnl_quote = +6 (s·(p_exec-p_int)·q)', near(s.pnl_quote, 6));
check('fills = 2', s.trade_fills === 2);

// Знаки в отдельности: чистый SELL (short base, +quote, прибыль если p_exec>p_int)
const sell = combineTradeStats({ sell_base: 1, sell_notional: 100, sell_pnlbase: 5, buy_base: 0, buy_notional: 0, buy_pnlbase: 0, fills: 1 });
check('pure SELL: base<0', sell.traded_base === -1);
check('pure SELL: quote>0', sell.traded_quote === 100);
check('pure SELL: pnl>0 когда p_exec>p_int', sell.pnl_quote === 5);
const buy = combineTradeStats({ buy_base: 1, buy_notional: 100, buy_pnlbase: 5, sell_base: 0, sell_notional: 0, sell_pnlbase: 0, fills: 1 });
check('pure BUY: base>0', buy.traded_base === 1);
check('pure BUY: quote<0', buy.traded_quote === -100);
check('pure BUY: pnl знак инвертирован (−5)', buy.pnl_quote === -5);

// Конверсия в USDT: */USDT (price==basePrice ⇒ ×1)
check('USDT pair: pnl_usdt == pnl_quote', pnlQuoteToUsdt(6, 2500, 2500) === 6);
// Кросс ETH/BTC: basePrice=P_ETH=2500 USDT, price=ETH/BTC=0.05 ⇒ ×(2500/0.05=50000=P_BTC)
check('cross ETH/BTC: pnl_usdt = pnl_btc * P_BTC', near(pnlQuoteToUsdt(0.01, 2500, 0.05), 0.01 * 50000));
// Нет цены ⇒ ×1 (не обнуляем)
check('no price ⇒ ×1', pnlQuoteToUsdt(6, 0, 0) === 6);
// Отрицательный заработок проходит как есть
check('negative pnl сохраняется', pnlQuoteToUsdt(-7, 2500, 2500) === -7);

if (failed > 0) { console.error(`\n${failed} FAILED`); process.exit(1); }
console.log('\n[OK] trade_stats_math: all passed');
