// Затраты хеджа: модель против рабочего правила на ИЗМЕРЕННЫХ параметрах агентов (выжимка эксперта, раздел 3).
// Вход — вывод shadow_summary.sql (разделитель «|», с заголовком). Расчёт — ядро страницы (he_engine.js), единицы как на странице.
//   psql ... -A -F '|' -f shadow_summary.sql | node tools/hedge-threshold-model/shadow_costs.js [--fm 10 --ft 10 --half 0.5 --adv 0.5 --kappa 1 --gamma 0.0047 --k 1.8 --floor 2]
'use strict';
const fs = require('fs');
const HE = require('./he_engine.js');

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? Number(process.argv[i + 1]) : d; };
const FM = arg('fm', 10), FT = arg('ft', 10), HALF = arg('half', 0.5), ADV = arg('adv', 0.5), KAPPA = arg('kappa', 1), GAMMA = arg('gamma', 0.0047);
const K = arg('k', 1.8), FLOOR = arg('floor', 2), SZ_DEFAULT = arg('szdef', 2);

const lines = fs.readFileSync(0, 'utf8').split('\n').filter((l) => l.includes('|') && !/^\(\d+ row/.test(l));
const head = lines.shift().split('|');
const rows = lines.map((l) => Object.fromEntries(l.split('|').map((v, i) => [head[i], v])));
const num = (v) => (v === '' || v === undefined ? NaN : Number(v));
const fmt = (x, d = 1) => (Number.isFinite(x) ? x.toFixed(d) : '—');

const out = [];
out.push(`Комиссии ${FM}/${FT} bps, полуспред ${HALF} bps, неблагоприятный отбор ${ADV} bps, κ = ${KAPPA}, γ = ${GAMMA}; затраты — USDT в час на агента; «правило» — z_lim/z_mkt = max(${FLOOR}, ${K}·комиссия).`);
out.push('');
out.push('| Агент | Минут | ς, bps/√мин | ς_z | Дрейф | Режимы (шум/смесь/дрейф) | Порог модели (мейкер / тейкер) | Рабочий порог | Затраты модели | Затраты правила | Правило дороже | Макс. позиция | Доля времени выше порога тейкера |');
out.push('|---|---:|---:|---:|---:|---|---|---|---:|---:|---:|---:|---:|');
let sumM = 0, sumP = 0, n = 0;
for (const r of rows) {
  const vol = num(r.sigma_bps);
  const sz = Number.isFinite(num(r.sz_est)) ? num(r.sz_est) : SZ_DEFAULT;
  if (!(vol > 0)) continue;
  const P = { A: HE.riskA(GAMMA, vol), sz, theta: 0, kappa: KAPPA, cm: 0.1 * (FM - HALF + ADV), ct: 0.1 * (FT + HALF), h: 0 };
  const m = HE.mtOptimize(P);
  const zlim = Math.max(FLOOR, K * FM), zmkt = Math.max(zlim, K * FT);
  const Jm = m.J * 60, Jp = HE.mtCost(Math.min(zlim, zmkt), zmkt, P).J * 60;
  sumM += Jm; sumP += Jp; n++;
  out.push(`| ${r.agent_id} | ${r.minutes} | ${fmt(vol, 2)} | ${fmt(sz, 2)}${Number.isFinite(num(r.sz_est)) ? '' : '*'} | ${fmt(num(r.mu_est), 3)} | ${r.share_noise}/${r.share_mixed}/${r.share_drift} | ${m.makerZone ? fmt(m.bm) + ' / ' + fmt(m.bt) : 'зоны мейкера нет: ' + fmt(m.bt)} | ${fmt(num(r.z_lim))} / ${fmt(num(r.z_mkt))} | ${fmt(Jm, 2)} | ${fmt(Jp, 2)} | ${fmt(100 * (Jp / Jm - 1), 0)} % | ${fmt(num(r.max_abs_pos))} | ${fmt(100 * num(r.share_above_zmkt), 0)} % |`);
}
out.push('');
out.push(`Итого по ${n} агентам: модель ${fmt(sumM, 2)} USDT/ч, правило ${fmt(sumP, 2)} USDT/ч (правило дороже на ${fmt(100 * (sumP / sumM - 1), 0)} %). «*» — оценка шума ещё не накоплена, взято значение по умолчанию ${SZ_DEFAULT}.`);
console.log(out.join('\n'));
