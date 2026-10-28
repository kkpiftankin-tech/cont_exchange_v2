'use strict';
// Сделки на внешних биржах по паре валют во времени (график на вкладке «PnL», CR-F18-006, CALC-CE-PAIR-TRADES).
// Источник — состоявшиеся хедж-заявки агентов CE (child_orders). SQL в server.js отдаёт БЕЗЗНАКОВЫЕ суммы по (пара, интервал); здесь —
// чистые функции без ввода-вывода: размер интервала по окну, средневзвешенные цены покупки и продажи, накопленные объёмы базовой валюты
// (общий, нетто, покупки, продажи). Браузер только рисует готовые значения: никаких вычислений на фронте.

const finite = (v) => v !== null && v !== undefined && Number.isFinite(Number(v));
const num = (v) => (finite(v) ? Number(v) : 0);

// Размер интервала по длине окна: 1 ч — 1 мин; 6 ч — 5 мин; 24 ч — 15 мин; больше суток — 1 ч.
function bucketMsForHours(hours) {
  const h = Number(hours);
  if (!(h > 0)) return 900000;
  if (h <= 1) return 60000;
  if (h <= 6) return 300000;
  if (h <= 24) return 900000;
  return 3600000;
}

// 'BTC/USDT' → { base: 'BTC', quote: 'USDT' }; символ без косой черты — база весь символ, вторая валюта неизвестна.
function splitPair(symbol) {
  const s = String(symbol || '');
  const i = s.indexOf('/');
  return i > 0 ? { base: s.slice(0, i), quote: s.slice(i + 1) } : { base: s, quote: '' };
}

// Средневзвешенная цена: сумма (количество × цена) / сумма количества; нет количества — null (не ноль).
const vwap = (notional, qty) => (num(qty) > 0 ? num(notional) / num(qty) : null);

// Ставки расчётной комиссии хеджа, б.п., по умолчанию — те же, что у ledger (CE_BAND_MAKER_FEE_BPS = 2, CE_BAND_REF_FEE_BPS = 10).
const DEFAULT_MAKER_BPS = 2;
const DEFAULT_TAKER_BPS = 10;

// Разбор ставок: row — строка f05a_clearing_config { ce_maker_fee_bps, ce_taker_fee_bps } или null; env — { CE_BAND_MAKER_FEE_BPS, CE_BAND_REF_FEE_BPS }.
// Ставка из конфигурации действует, только если больше нуля («не больше нуля» — не задана, как в ledger); иначе env (если больше нуля), иначе значение по
// умолчанию. source: 'config' — обе ставки из конфигурации; 'default' — ни одной; 'mixed' — одна из конфигурации, другая по умолчанию или из env.
function resolveFeeBps(row, env) {
  const e = env || {};
  const pick = (cfg, envVal, def) => (Number(cfg) > 0 ? { v: Number(cfg), cfg: true } : { v: Number(envVal) > 0 ? Number(envVal) : def, cfg: false });
  const maker = pick(row && row.ce_maker_fee_bps, e.CE_BAND_MAKER_FEE_BPS, DEFAULT_MAKER_BPS);
  const taker = pick(row && row.ce_taker_fee_bps, e.CE_BAND_REF_FEE_BPS, DEFAULT_TAKER_BPS);
  const source = maker.cfg && taker.cfg ? 'config' : (!maker.cfg && !taker.cfg ? 'default' : 'mixed');
  return { maker_bps: maker.v, taker_bps: taker.v, source };
}

// Ставки расчётной комиссии, б.п.: maker — лимитные заявки (пассивные), taker — рыночные; источник: 'config', 'mixed' или 'default' (resolveFeeBps).
const normFee = (fee) => ({
  maker_bps: finite(fee && fee.maker_bps) ? Number(fee.maker_bps) : null,
  taker_bps: finite(fee && fee.taker_bps) ? Number(fee.taker_bps) : null,
  source: fee && fee.source ? String(fee.source) : 'default',
});

// rows — строки SQL по (symbol, t): { symbol, t, buy_qty, sell_qty, buy_notional, sell_notional, buy_fee, fills }; t — начало интервала, мс;
// buy_fee — расчётная комиссия покупок интервала во второй валюте пары (SQL: стоимость × ставка по типу заявки, CALC-CE-PAIR-TRADES §2.1).
// fee — ставки, по которым она посчитана (попадают в ответ). Возвращает DTO графика: пары (по убыванию числа исполнений, затем по имени), у пары
// точки по возрастанию t с ценами интервала (в том числе покупки с комиссией) и накопленными от начала окна объёмами, плюс итоги пары за окно.
function buildPairTradesDto(rows, hours, nowMs, bucketMs, fee) {
  const feeInfo = normFee(fee);
  const feeKnown = feeInfo.maker_bps !== null && feeInfo.taker_bps !== null;   // ставок нет — комиссия неизвестна (null), а не нулевая
  const bucket = Number(bucketMs) > 0 ? Number(bucketMs) : bucketMsForHours(hours);
  const by = new Map();
  for (const r of Array.isArray(rows) ? rows : []) {
    if (!r || !r.symbol || !finite(r.t)) continue;
    if (!by.has(r.symbol)) by.set(r.symbol, []);
    by.get(r.symbol).push(r);
  }
  const pairs = [];
  let firstMs = null;
  for (const [symbol, list] of by) {
    list.sort((x, y) => Number(x.t) - Number(y.t));
    let cb = 0, cs = 0, fills = 0, nb = 0, ns = 0, fb = 0;
    const points = [];
    for (const r of list) {
      const bq = num(r.buy_qty), sq = num(r.sell_qty);
      if (!(bq > 0) && !(sq > 0)) continue;   // интервал без исполнений в ответ не идёт
      cb += bq; cs += sq; nb += num(r.buy_notional); ns += num(r.sell_notional); fb += num(r.buy_fee); fills += Math.trunc(num(r.fills));
      points.push({
        t: Number(r.t),
        buy_qty: bq, sell_qty: sq,
        buy_price: vwap(r.buy_notional, bq), sell_price: vwap(r.sell_notional, sq),
        buy_price_fee: feeKnown ? vwap(num(r.buy_notional) + num(r.buy_fee), bq) : null,   // цена покупки с комиссией: цена + комиссия на единицу базовой валюты
        cum_buy_qty: cb, cum_sell_qty: cs, cum_total_qty: cb + cs, cum_net_qty: cb - cs,
      });
    }
    if (!points.length) continue;
    if (firstMs === null || points[0].t < firstMs) firstMs = points[0].t;
    const { base, quote } = splitPair(symbol);
    pairs.push({
      pair: symbol, base, quote, fills,
      totals: { buy_qty: cb, sell_qty: cs, total_qty: cb + cs, net_qty: cb - cs, buy_price: vwap(nb, cb), sell_price: vwap(ns, cs), buy_price_fee: feeKnown ? vwap(nb + fb, cb) : null, buy_fee: feeKnown ? fb : null },
      points,
    });
  }
  pairs.sort((a, b) => (b.fills - a.fills) || a.pair.localeCompare(b.pair));
  return {
    window_hours: hours,
    bucket_ms: bucket,
    since_ms: Number.isFinite(Number(nowMs)) ? Number(nowMs) - hours * 3600 * 1000 : null,
    first_ms: firstMs,
    fee: feeInfo,
    pairs,
    has_data: pairs.length > 0,
  };
}

module.exports = { bucketMsForHours, splitPair, buildPairTradesDto, resolveFeeBps, DEFAULT_MAKER_BPS, DEFAULT_TAKER_BPS };
