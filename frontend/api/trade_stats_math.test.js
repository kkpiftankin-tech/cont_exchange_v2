// Self-contained node-тест (у frontend/api нет jest): `node trade_stats_math.test.js`.
// Проверяет знаки количеств по состоявшимся сделкам (BUY +base/−quote, SELL −base/+quote). Заработок (R, U) здесь не считается —
// он приходит из ledger (ADR-087 п. 10), см. agent_pnl_dto.test.js.
const assert = require('assert');
const { combineTradeStats, totalPosition, arbPositionParts } = require('./trade_stats_math');

let failed = 0;
const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}

// Числовой пример (T_ETH ETH/USDT): Fill1 SELL 0.4 @2510, Fill2 BUY 0.2 @2470 — безнаковые суммы по сторонам, как из SQL.
const s = combineTradeStats({ buy_base: 0.2, sell_base: 0.4, buy_notional: 0.2 * 2470, sell_notional: 0.4 * 2510, fills: 2 });
// Δbase = 0.2 − 0.4 = −0.2; Δquote = 1004 − 494 = +510
check('base = -0.2 (BUY +, SELL -)', near(s.traded_base, -0.2));
check('quote = +510 (SELL +, BUY -)', near(s.traded_quote, 510));
check('fills = 2', s.trade_fills === 2);
check('заработок здесь не считается: нет полей R/U', s.pnl_quote === undefined && s.realized_quote === undefined);

// Знаки в отдельности
const sell = combineTradeStats({ sell_base: 1, sell_notional: 100, buy_base: 0, buy_notional: 0, fills: 1 });
check('pure SELL: base<0', sell.traded_base === -1);
check('pure SELL: quote>0', sell.traded_quote === 100);
const buy = combineTradeStats({ buy_base: 1, buy_notional: 100, sell_base: 0, sell_notional: 0, fills: 1 });
check('pure BUY: base>0', buy.traded_base === 1);
check('pure BUY: quote<0', buy.traded_quote === -100);
check('нечисловые поля считаются нулём', combineTradeStats({ buy_base: 'x', fills: undefined }).trade_fills === 0);

// --- Полная позиция агента (c + in_flight), как на вкладке «Граф»
check('totalPosition: c + in_flight', near(totalPosition(0.2452, 0.5506), 0.7958));
check('totalPosition: нет заявок в полёте — просто c', near(totalPosition(-1.11, 0), -1.11) && near(totalPosition(-1.11, undefined), -1.11));
check('totalPosition: нет позиции — null, а не 0', totalPosition(null, 0.5) === null && totalPosition(undefined, 0) === null && totalPosition('x', 1) === null);
check('totalPosition: знаки одинаковые у c и in_flight (отрицательная позиция)', near(totalPosition(-2.0, -0.55), -2.55));

// --- Позиция арбитражёра: открытая позиция и базис раздельно
{
  const parts = (...x) => arbPositionParts(...x);
  // зеркальная позиция: q_src = −q_dst ⇒ открытой позиции нет, весь результат — базис q_dst·(P_dst − P_src)
  const m = parts('ETH', 'USDT', 1.7579, 2398.80, -1.7579, 2398.66);
  check('зеркальные ноги: открытая позиция 0', near(m.open_position_usdt, 0, 1e-9));
  check('зеркальные ноги: базис = q_dst·(P_dst−P_src) = −1.7579·(−0.14)', near(m.basis_usdt, -1.7579 * (2398.66 - 2398.80), 1e-9));
  check('длинная нога на дорогой бирже даёт плюс базиса', parts('BTC', 'USDT', -0.022, 81819.45, 0.022, 81829.05).basis_usdt > 0);
  // несимметричные ноги (A_BTC_coinbaseokx с dev): обе короткие ⇒ чистая короткая позиция по активу
  const n = parts('BTC', 'USDT', -0.132, 81825.0, -0.056, 81826.0);
  check('обе ноги короткие: открытая позиция ≈ −0.188 BTC × цена', near(n.open_position_usdt, -0.188 * 81825.5, 1e-6));
  check('сумма частей равна стоимости ног q_src·P_src + q_dst·P_dst',
    near(n.open_position_usdt + n.basis_usdt, -0.132 * 81825.0 + -0.056 * 81826.0, 1e-6));
  const r = parts('SOL', 'USDT', 5, 104.69, -3, 104.70);
  check('произвольные ноги: части в сумме дают стоимость', near(r.open_position_usdt + r.basis_usdt, 5 * 104.69 - 3 * 104.70, 1e-9));
  check('перенос нумерария (USDT): обе части 0', parts('USDT', 'USDT', 1209.5, 1, -1209.5, 1).open_position_usdt === 0 && parts('USDT', 'USDT', 5, 1, -5, 1).basis_usdt === 0);
  check('нет цены => null', parts('SOL', 'USDT', -5, 0, 5, 104.7) === null && parts('SOL', 'USDT', -5, 104.7, 5, null) === null);
  check('нет количества => null', parts('SOL', 'USDT', null, 104.7, 5, 104.7) === null);
  check('одинаковые цены и зеркальные ноги => 0', near(parts('BTC', 'USDT', -1, 80000, 1, 80000).basis_usdt, 0) && near(parts('BTC', 'USDT', -1, 80000, 1, 80000).open_position_usdt, 0));
}

if (failed > 0) { console.error(`\n${failed} FAILED`); process.exit(1); }
console.log('\n[OK] trade_stats_math: all passed');

// --- combineTradeStatsByVenue: итог по агенту и разбивка по биржам (арбитражёр: источник и приёмник)
{
  const { combineTradeStatsByVenue } = require('./trade_stats_math');
  const rows = [
    // арбитражёр: купил 1 BTC на src по 100, продал 1 BTC на dst по 102
    { agent_id: 'A_BTC_srcdst', venue: 'src', buy_base: 1, sell_base: 0, buy_notional: 100, sell_notional: 0, fills: 1 },
    { agent_id: 'A_BTC_srcdst', venue: 'dst', buy_base: 0, sell_base: 1, buy_notional: 0, sell_notional: 102, fills: 1 },
    { agent_id: 'T_BTC_x', venue: 'x', buy_base: 2, sell_base: 0, buy_notional: 200, sell_notional: 0, fills: 4 },
    { venue: 'orphan' },
  ];
  const m = combineTradeStatsByVenue(rows);
  const a = m.get('A_BTC_srcdst');
  console.log(a && a.trade_fills === 2 && a.traded_base === 0 && a.traded_quote === 2 ? '[OK] арбитражёр: итог по двум биржам (база 0, квота +2)' : '[FAIL] арбитражёр итог');
  console.log(a && a.by_venue.src.traded_base === 1 && a.by_venue.src.traded_quote === -100 && a.by_venue.dst.traded_base === -1 && a.by_venue.dst.traded_quote === 102
    ? '[OK] разбивка: src +1 база/−100 квота, dst −1 база/+102 квота' : '[FAIL] by_venue');
  const t1 = m.get('T_BTC_x');
  console.log(t1 && t1.traded_base === 2 && t1.by_venue.x.trade_fills === 4 ? '[OK] переводчик: одна биржа' : '[FAIL] переводчик');
  console.log(m.size === 2 ? '[OK] строка без agent_id пропущена' : '[FAIL] orphan');
  console.log(combineTradeStatsByVenue(null).size === 0 ? '[OK] не массив → пусто' : '[FAIL] null');
  if (!(a && a.trade_fills === 2 && t1 && m.size === 2)) process.exit(1);
}

