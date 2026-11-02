// Эталонные значения для C++ порта модели порогов хеджа (cpp/ledger/src/domain/hedge_threshold_model.hpp).
//   node tools/hedge-threshold-model/gen_golden.js
// Берёт расчётное ядро страницы (he_engine.js, дословно) и считает набор случаев в тех же единицах, что и
// страница (позиция — тыс. USDT, время — минуты, издержки — USDT на 1 тыс. USDT = 0,1 × bps). Пишет:
//   golden.json                                          — для эксперта и для сверки;
//   cpp/ledger/tests/hedge_threshold_model_golden.inc    — таблицы для юнит-теста порта.
'use strict';
const fs = require('fs');
const path = require('path');
const HE = require('./he_engine.js');

const OUT_JSON = path.join(__dirname, 'golden.json');
const OUT_INC = path.join(__dirname, '..', '..', 'cpp', 'ledger', 'tests', 'hedge_threshold_model_golden.inc');

// Параметры страницы по умолчанию (главы 1–7).
const D = { gamma: 0.0047, sigma: 4.8, ft: 10, sh: 0.5, fm: 2, adv: 0.5, kappa: 1, sz: 2, theta: 0,
            sigb: 0.7, szl: 2, K: 2, ctr: 0, hcap: 10, Td: 0 };
const mk = (name, o) => Object.assign({ name }, D, o);

const MT = [
  mk('default_2_10', {}),
  mk('standard_fee_10_10', { fm: 10 }),
  mk('taker_only', { kappa: 0 }),
  mk('rebate_maker', { fm: -1 }),
  mk('fast_maker', { kappa: 20 }),
  mk('slow_maker', { kappa: 0.05 }),
  mk('weak_return', { theta: 0.001 }),
  mk('strong_return', { theta: 0.1 }),
  mk('high_noise', { sz: 6 }),
  mk('low_vol', { sigma: 1.5 }),
  mk('high_gamma', { gamma: 0.02 }),
  mk('maker_not_cheaper', { fm: 20 }),
  mk('zero_taker_fee', { ft: 0 }),
];
const TR = [
  mk('transfer_default', {}),
  mk('transfer_cheap_network', { K: 0.05 }),
  mk('transfer_expensive_network', { K: 200 }),
  mk('transfer_with_percent', { ctr: 2.5 }),
  mk('transfer_with_delay', { Td: 30 }),
  mk('transfer_no_capital_cost', { hcap: 0 }),
  mk('transfer_with_return', { theta: 0.01 }),
];
const DR = [0.01, 0.1, 1, 10].map((mu) => ({ name: 'drift_mu_' + mu, mu }));
const BT = [
  { name: 'batch_default', mu: 0.5, K: 0.5, h: 0 },
  { name: 'batch_with_capital', mu: 0.5, K: 0.5, h: 1000 * 10 / 100 / 525600 },
  { name: 'batch_fast_flow', mu: 10, K: 2, h: 0 },
];

const units = (p) => {
  const A = HE.riskA(p.gamma, p.sigma), Ab = HE.riskA(p.gamma, p.sigb);
  const ct = 0.1 * (p.ft + p.sh), cm = 0.1 * (p.fm - p.sh + p.adv);
  const h = 1000 * p.hcap / 100 / 525600;
  return { A, Ab, ct, cm, h };
};

const out = { mt: [], tr: [], drift: [], batch: [], closed: [] };
for (const p of MT) {
  const u = units(p);
  const P = { A: u.A, sz: p.sz, theta: p.theta, kappa: p.kappa, cm: u.cm, ct: u.ct, h: 0 };
  const r = HE.mtOptimize(P);
  out.mt.push({ name: p.name, in: P, out: { makerZone: r.makerZone, bm: r.makerZone ? r.bm : 0, bt: r.bt, J: r.J, risk: r.risk, maker: r.maker, taker: r.taker, W0: r.W0 } });
}
for (const p of TR) {
  const u = units(p);
  const P = { A: u.Ab, sz: p.szl, theta: p.theta, h: u.h, K: p.K, ctr: 0.1 * p.ctr, Td: p.Td };
  const r = HE.trOptimize(P);
  out.tr.push({ name: p.name, in: P, out: { B: r.B, b: r.b, J: r.J, T: r.T, risk: r.risk, fee: r.fee, perHour: r.perHour } });
}
{
  const u = units(D);
  for (const c of DR) {
    const P = { A: u.A, sz: D.sz, mu: c.mu, ct: u.ct };
    const r = HE.driftOptimize(P);
    out.drift.push({ name: c.name, in: P, out: { b: r.b, J: r.J, fee: r.fee, risk: r.risk, Pe: r.Pe } });
  }
  for (const c of BT) {
    const P = { A: u.A, mu: c.mu, K: c.K, h: c.h };
    const r = HE.batchOptimize(P);
    out.batch.push({ name: c.name, in: P, out: { H: r.H, J: r.J, interval: r.interval } });
  }
  // замкнутые формулы: (5) кубический корень и (10в) корень четвёртой степени
  out.closed.push({ name: 'cube_root_default', in: { c: u.ct, sz: D.sz, A: u.A }, out: { b: HE.cubeRootBand(u.ct, D.sz, u.A) } });
  out.closed.push({ name: 'quarter_root_default', in: { K: D.K, sz: D.szl, A: u.Ab }, out: { B: HE.quarterTrigger(D.K, D.szl, u.Ab) } });
}

fs.writeFileSync(OUT_JSON, JSON.stringify(out, null, 1) + '\n');

// --- C++ таблицы ---
const f = (x) => (Number.isFinite(x) ? Number(x).toPrecision(17) : '0.0');
const b = (x) => (x ? 'true' : 'false');
let inc = '// АВТОГЕНЕРАЦИЯ: node tools/hedge-threshold-model/gen_golden.js. Не править.\n' +
  '// Эталон — расчётное ядро страницы incoming-docs/2026-10-29-porogi-hedzha5-380dc48d.html (sha1-8 380dc48d).\n\n';
inc += 'struct GoldenMt { const char* name; double A, sz, theta, kappa, cm, ct; bool maker_zone; double bm, bt, J; };\n';
inc += 'static const GoldenMt kGoldenMt[] = {\n';
for (const c of out.mt) inc += `  {"${c.name}", ${f(c.in.A)}, ${f(c.in.sz)}, ${f(c.in.theta)}, ${f(c.in.kappa)}, ${f(c.in.cm)}, ${f(c.in.ct)}, ${b(c.out.makerZone)}, ${f(c.out.bm)}, ${f(c.out.bt)}, ${f(c.out.J)}},\n`;
inc += '};\n\n';
inc += 'struct GoldenTr { const char* name; double A, sz, theta, h, K, ctr, Td; double B, b, J, T; };\n';
inc += 'static const GoldenTr kGoldenTr[] = {\n';
for (const c of out.tr) inc += `  {"${c.name}", ${f(c.in.A)}, ${f(c.in.sz)}, ${f(c.in.theta)}, ${f(c.in.h)}, ${f(c.in.K)}, ${f(c.in.ctr)}, ${f(c.in.Td)}, ${f(c.out.B)}, ${f(c.out.b)}, ${f(c.out.J)}, ${f(c.out.T)}},\n`;
inc += '};\n\n';
inc += 'struct GoldenDrift { const char* name; double A, sz, mu, ct; double b, J, Pe; };\n';
inc += 'static const GoldenDrift kGoldenDrift[] = {\n';
for (const c of out.drift) inc += `  {"${c.name}", ${f(c.in.A)}, ${f(c.in.sz)}, ${f(c.in.mu)}, ${f(c.in.ct)}, ${f(c.out.b)}, ${f(c.out.J)}, ${f(c.out.Pe)}},\n`;
inc += '};\n\n';
inc += 'struct GoldenBatch { const char* name; double A, mu, K, h; double H, J, interval; };\n';
inc += 'static const GoldenBatch kGoldenBatch[] = {\n';
for (const c of out.batch) inc += `  {"${c.name}", ${f(c.in.A)}, ${f(c.in.mu)}, ${f(c.in.K)}, ${f(c.in.h)}, ${f(c.out.H)}, ${f(c.out.J)}, ${f(c.out.interval)}},\n`;
inc += '};\n\n';
inc += `static const double kGoldenCubeRoot = ${f(out.closed[0].out.b)};   // c=${f(out.closed[0].in.c)}, sz=${f(out.closed[0].in.sz)}, A=${f(out.closed[0].in.A)}\n`;
inc += `static const double kGoldenQuarterRoot = ${f(out.closed[1].out.B)};\n`;
fs.mkdirSync(path.dirname(OUT_INC), { recursive: true });
fs.writeFileSync(OUT_INC, inc);
console.log('golden.json: mt=%d tr=%d drift=%d batch=%d', out.mt.length, out.tr.length, out.drift.length, out.batch.length);
console.log('записан', path.relative(process.cwd(), OUT_INC));
