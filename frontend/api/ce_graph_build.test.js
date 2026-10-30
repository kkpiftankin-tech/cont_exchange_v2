// Self-contained node-тест: `node ce_graph_build.test.js`.
const assert = require('assert');
const { buildCeGraph, makePriceOf } = require('./ce_graph_build');
let failed = 0;
const near = (a, b, e = 1e-9) => Math.abs(a - b) <= e * Math.max(1, Math.abs(b));
const check = (n, c) => { if (c) console.log('[OK] ' + n); else { console.error('[FAIL] ' + n); failed++; } };

const segs = [
  { venue: 'v1', pair: 'BTC/USDT', mid: 80000, alphaT: 1000000, betaT: 0.64, eventMs: 1 },
  { venue: 'v2', pair: 'BTC/USDT', mid: 80080, alphaT: 500000, betaT: 1.28, eventMs: 1 },
  { venue: 'v1', pair: 'ETH/BTC', mid: 0.03, alphaT: 0.5, betaT: 1e-7, eventMs: 1 },
  { venue: 'v1', pair: 'ETH/USDT', mid: 2400, alphaT: 100000, betaT: 0.05, eventMs: 1 },
];
const positions = [
  { agent_id: 'T_BTC_v1', agent_kind: 'translator', asset: 'BTC', venue: 'v1', position: 100, in_flight: 20 },
  { agent_id: 'A_BTC_v1v2', agent_kind: 'arbitrageur', asset: 'BTC', venue: 'v1', position: -30, in_flight: 0 },
  { agent_id: 'A_BTC_v1v2', agent_kind: 'arbitrageur', asset: 'BTC', venue: 'v2', position: 30, in_flight: 0 },
];
const g = buildCeGraph({ segs, positions, cfg: { gamma: 0.005, clampPm: 8, transfer: { BTC: { alpha: 12, c: 0.25 } } } });

const t = g.edges.find((e) => e.id === 'T:v1:BTC/USDT');
check('переводчик: ребро USDT@v1 → BTC@v1', t && t.from === 'USDT@v1' && t.to === 'BTC@v1');
// c = 120 k-USDT, skew = 0.005·120 = 0.6‰, якорь сдвигается вниз
check('skew = γ·c = 0.6‰', near(t.skewPm, 0.6));
check('anchor_eff = mid·exp(−0.6/1000)', near(t.anchorEff, 80000 * Math.exp(-0.0006)));
check('r = 100/α (quote=USDT)', near(t.rBp100, 100 / 1e6));
check('позиция в USDT = c·1000', near(t.positionUsdt, 120000));

const x = g.edges.find((e) => e.id === 'T:v1:ETH/BTC');
check('кросс-пара: α в USDT = α_BTC·цена BTC', near(x.alphaUsdt, 0.5 * 80000) && near(x.rBp100, 100 / 40000));
check('кросс-пара без позиции: skew 0, hasAgent=false', x.skewPm === 0 && x.hasAgent === false);

const a = g.edges.find((e) => e.id === 'A:A_BTC_v1v2');
check('арбитражёр: BTC@v1 → BTC@v2', a && a.from === 'BTC@v1' && a.to === 'BTC@v2');
check('ЭДС = 1e4·ln(P2/P1)', near(a.emfBp, 1e4 * Math.log(80080 / 80000)));
check('r = 1/α_A, α = 100·α_A', near(a.rBp100, 1 / 12) && near(a.alphaUsdt, 1200));
check('ноги арбитражёра в USDT', near(a.legSrcUsdt, -30000) && near(a.legDstUsdt, 30000));

// потоки такта: переводчик f=−Δc·1000 (c>0 ⇒ база→quote), арбитражёр f=(Δdst−Δsrc)/2·1000
const gf = buildCeGraph({ segs, positions, cfg: { transfer: { BTC: { alpha: 12, c: 0.25 } } }, deltas: [
  { agent_id: 'T_BTC_v1', venue: 'v1', delta: 2 },
  { agent_id: 'A_BTC_v1v2', venue: 'v1', delta: -3 }, { agent_id: 'A_BTC_v1v2', venue: 'v2', delta: 3 },
] });
const tf = gf.edges.find((e) => e.id === 'T:v1:BTC/USDT'), af = gf.edges.find((e) => e.id === 'A:A_BTC_v1v2');
check('поток переводчика f = −Δc', near(tf.flowUsdt, -2000));
check('поток арбитражёра f = (Δdst−Δsrc)/2: src→dst', near(af.flowUsdt, 3000));
check('ветвь без дельт: поток 0', gf.edges.find((e) => e.id === 'T:v1:ETH/BTC').flowUsdt === 0);
// BTC@v2: только входящая арбитражная ветвь (+3000); BTC@v1: приток −2000 от переводчика и отток 3000 по арбитражу
check('приток узла BTC@v2 = +3000', near(gf.nodes.find((n) => n.id === 'BTC@v2').netFlowUsdt, 3000));
check('приток узла BTC@v1 = −2000 − 3000', near(gf.nodes.find((n) => n.id === 'BTC@v1').netFlowUsdt, -5000));
// источник данных: живая быстрая полоса помечается, остальное — клиринг
const gs = buildCeGraph({ segs: [{ ...segs[0], source: 'live', levelsBid: 1000, levelsAsk: 1000 }, segs[1]], positions: [], cfg: {} });
check('источник live сохраняется', gs.edges.find((e) => e.id === 'T:v1:BTC/USDT').source === 'live' && gs.edges.find((e) => e.id === 'T:v1:BTC/USDT').levelsBid === 1000);
check('по умолчанию источник — клиринг', gs.edges.find((e) => e.id === 'T:v2:BTC/USDT').source === 'clearing');
// клэмп skew
const g2 = buildCeGraph({ segs, positions: [{ agent_id: 'T_BTC_v1', agent_kind: 'translator', asset: 'BTC', venue: 'v1', position: 5000, in_flight: 0 }], cfg: { gamma: 0.005, clampPm: 8 } });
check('skew ограничен клэмпом', near(g2.edges.find((e) => e.id === 'T:v1:BTC/USDT').skewPm, 8));
// без позиций и γ: якорь = mid
const g3 = buildCeGraph({ segs, positions: [], cfg: {} });
check('без γ якорь не сдвигается', g3.edges.every((e) => e.kind !== 'translator' || e.anchorEff === e.anchor));
check('узлы и списки', g.nodes.some((n) => n.id === 'BTC@v2' && near(n.priceUsdt, 80080)) && g.pairs.length === 3 && g.venues.join() === 'v1,v2');
// мусор не ломает
check('мусорные сегменты пропущены', buildCeGraph({ segs: [null, { venue: 'x', pair: 'bad', mid: 1 }, { venue: 'x', pair: 'A/B', mid: 0 }] }).edges.length === 0);
// ── Позиции у концов ветви (ends): в валютах концов, со знаком ──
// Переводчик T_BTC_v1: c = 100+20 = 120 k-USDT ⇒ v = 120000 USDT. База: −v/P_base; quote(USDT): +v/1.
const posL = [
  { agent_id: 'T_BTC_v1', agent_kind: 'translator', asset: 'BTC', venue: 'v1', position: 100, in_flight: 20, base_price: 80000, reference_price: 80000 },
  { agent_id: 'A_BTC_v1v2', agent_kind: 'arbitrageur', asset: 'BTC', venue: 'v1', position: -30, in_flight: 0 },
  { agent_id: 'A_BTC_v1v2', agent_kind: 'arbitrageur', asset: 'BTC', venue: 'v2', position: 30, in_flight: 0 },
  // кросс-пара ETH/BTC: P_quote(BTC) = base_price/reference_price = 2400/0.03 = 80000
  { agent_id: 'T_ETH_BTC_v1', agent_kind: 'translator', asset: 'ETH', venue: 'v1', position: 60, in_flight: 0, base_price: 2400, reference_price: 0.03 },
];
const ge = buildCeGraph({ segs, positions: posL, cfg: { transfer: { BTC: { alpha: 12, c: 0.25 } } } });
const te = ge.edges.find((e) => e.id === 'T:v1:BTC/USDT');
check('переводчик: у узла BTC позиция −v/P_base (короткая по базе)', te.ends.to.asset === 'BTC' && near(te.ends.to.qty, -120000 / 80000));
check('переводчик: у узла USDT позиция +v (нотная нога)', te.ends.from.asset === 'USDT' && near(te.ends.from.qty, 120000));
const xe = ge.edges.find((e) => e.id === 'T:v1:ETH/BTC');
check('кросс-пара: база ETH = −v/P_ETH, quote BTC = +v/P_BTC (P_BTC = base_price/ref)', near(xe.ends.to.qty, -60000 / 2400) && xe.ends.from.asset === 'BTC' && near(xe.ends.from.qty, 60000 / 80000));
const ae = ge.edges.find((e) => e.id === 'A:A_BTC_v1v2');
check('арбитражёр: ноги по биржам в единицах актива (−0,375 / +0,375 BTC)', near(ae.ends.from.qty, -30000 / 80000) && near(ae.ends.to.qty, 30000 / 80080));
check('цены ног арбитражёра — свои на каждой бирже', ae.ends.from.asset === 'BTC' && ae.ends.to.asset === 'BTC');
check('нет строки агента ⇒ позиции нет (null), а не ноль', buildCeGraph({ segs, positions: [], cfg: {} }).edges.find((e) => e.id === 'T:v1:BTC/USDT').ends === null);
// без цен ledger — берём цены сегментов клиринга
const gf2 = buildCeGraph({ segs, positions: [{ agent_id: 'T_BTC_v1', agent_kind: 'translator', asset: 'BTC', venue: 'v1', position: 100, in_flight: 0 }], cfg: {} });
check('нет base_price: цены из сегментов (80000)', near(gf2.edges.find((e) => e.id === 'T:v1:BTC/USDT').ends.to.qty, -100000 / 80000));

// ── Позиция на внешней бирже от хеджирования ──
// BTC/USDT на v1: переводчик купил 0,5 BTC за 40 000 USDT и продал 0,2 BTC за 16 100 USDT;
// арбитражёр на v2 продал 1 BTC за 80 000 USDT. ETH/BTC на v1: переводчик купил 3 ETH за 0,09 BTC.
const hf = [
  { agent_id: 'T_BTC_v1', venue: 'v1', symbol: 'BTCUSDT', buy_base: 0.5, sell_base: 0.2, buy_notional: 40000, sell_notional: 16100 },
  { agent_id: 'A_BTC_v1v2', venue: 'v2', symbol: 'BTCUSDT', buy_base: 0, sell_base: 1, buy_notional: 0, sell_notional: 80000 },
  { agent_id: 'T_ETH_BTC_v1', venue: 'v1', symbol: 'ETHBTC', buy_base: 3, sell_base: 0, buy_notional: 0.09, sell_notional: 0 },
  { agent_id: 'T_XRP_v1', venue: 'v1', symbol: 'XRPUSDT', buy_base: 10, sell_base: 0, buy_notional: 6, sell_notional: 0 },   // XRP/USDT нет в графе: разбор по суффиксу
  { agent_id: 'T_X', venue: 'v1', symbol: 'ZZZ', buy_base: 1, sell_base: 0, buy_notional: 1, sell_notional: 0 },                // не разбирается — пропущен
];
const gh = buildCeGraph({ segs, positions: posL, hedgeFills: hf, cfg: { transfer: { BTC: { alpha: 12, c: 0.25 } } } });
const nh = (id) => gh.nodes.find((n) => n.id === id).hedgePos;
check('BTC@v1: 0,5 − 0,2 − 0,09 (ушло в ETH/BTC) = +0,21', near(nh('BTC@v1').qty, 0.3 - 0.09));
check('BTC@v1 — вклад переводчиков', near(nh('BTC@v1').translator, 0.21) && nh('BTC@v1').arbitrageur === 0);
check('USDT@v1: −40000 + 16100 − 6 = −23906', near(nh('USDT@v1').qty, -40000 + 16100 - 6));
check('BTC@v2: арбитражёр продал 1 BTC ⇒ −1', near(nh('BTC@v2').qty, -1) && near(nh('BTC@v2').arbitrageur, -1) && nh('BTC@v2').translator === 0);
check('USDT@v2: выручка +80000, вклад арбитражёра', near(nh('USDT@v2').qty, 80000) && near(nh('USDT@v2').arbitrageur, 80000));
check('кросс-пара: ETH@v1 +3 ', near(nh('ETH@v1').qty, 3));
check('узел без хеджей — нулевая позиция, а не null', (() => { const z = buildCeGraph({ segs, positions: [], hedgeFills: [], cfg: {} }).nodes[0].hedgePos; return z && z.qty === 0; })());
check('сумма по узлам сходится: Σ USDT-ноги = −Σ(buy_notional − sell_notional) по парам к USDT', near(
  gh.nodes.filter((n) => n.asset === 'USDT').reduce((s, n) => s + n.hedgePos.qty, 0), -40000 + 16100 + 80000 - 6));

// ── Позиции агентов на внутренней CE-бирже по валюте узла (node.cePos) ──
// Данные posL / segs выше: T_BTC_v1 (c=120k ⇒ BTC −1,5; USDT +120000), арбитражёр A_BTC_v1v2 (BTC@v1 −0,375; BTC@v2 +30000/80080),
// T_ETH_BTC_v1 (c=60k ⇒ ETH −25; BTC@v1 +0,75).
const nce = (id) => ge.nodes.find((n) => n.id === id).cePos;
check('BTC@v1: −1,5 (переводчик BTC) + 0,75 (ETH/BTC, quote) − 0,375 (арбитражёр) = −1,125', near(nce('BTC@v1').qty, -1.5 + 0.75 - 0.375));
check('BTC@v1: вклад переводчиков −0,75, арбитражёра −0,375', near(nce('BTC@v1').translator, -0.75) && near(nce('BTC@v1').arbitrageur, -0.375));
check('BTC@v2: нога dst арбитражёра +30000/80080', near(nce('BTC@v2').qty, 30000 / 80080) && nce('BTC@v2').translator === 0);
check('USDT@v1: нотная нога переводчика +120000', near(nce('USDT@v1').qty, 120000));
check('ETH@v1: −60000/2400 = −25', near(nce('ETH@v1').qty, -25));
check('узел без агентов с позициями: ноль, а не null', (() => { const z = buildCeGraph({ segs, positions: [], cfg: {} }).nodes[0].cePos; return z && z.qty === 0 && z.translator === 0; })());
// стоимостная сходимость: Σ(cePos·цена) по узлам ≈ 0 для замкнутых пар (переводчик: −v + v; арбитражёр: −leg + leg по своим ценам)
const valSum = ge.nodes.reduce((sum, n) => sum + n.cePos.qty * (n.priceUsdt || 0), 0);
check('Σ позиций агентов в USDT-стоимости по узлам ≈ 0 (замкнутость), |Σ| < 20 USDT на разницу цен BTC между биржами', Math.abs(valSum) < 20);


// --- Цена актива по бирже (общая для графа и панели «Живые позиции»)
{
  const priceOf = makePriceOf(segs);
  check('priceOf: USDT = 1', priceOf('USDT', 'v1') === 1);
  check('priceOf: прямая пара — своя цена на каждой бирже', priceOf('BTC', 'v1') === 80000 && priceOf('BTC', 'v2') === 80080);
  check('priceOf: разница цен между биржами сохраняется (иначе базис арбитражёра был бы 0)', priceOf('BTC', 'v2') - priceOf('BTC', 'v1') === 80);
  check('priceOf: нет данных — null', priceOf('XRP', 'v1') === null && priceOf('BTC', 'нет-такой-биржи') === null);
  check('priceOf: пустые сегменты — null, без падения', makePriceOf([])('BTC', 'v1') === null && makePriceOf(undefined)('BTC', 'v1') === null);
}

assert.strictEqual(failed, 0, failed + ' failed');
console.log('ce_graph_build tests passed');
