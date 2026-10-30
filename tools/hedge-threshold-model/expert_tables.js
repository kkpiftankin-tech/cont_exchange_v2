// Таблицы для выжимки эксперта (docs/experiments/EXP-2026-10-29-hedge-threshold-model.md): пороги модели против правила проекта
// на реальных волатильностях и полуспредах стенда. Расчёт — ядро страницы (he_engine.js), единицы как на странице.
//   node tools/hedge-threshold-model/expert_tables.js > /tmp/tables.md
'use strict';
const HE = require('./he_engine.js');

// Исходные данные стенда 2026-10-29 (источники: ce_asset_volatility, ClickHouse venue_snapshots за 6 часов, медиана):
// волатильность курса пары, bps за √мин = σ(за √с) · 1e4 · √60; полуспред, bps (OKX — основная площадка хеджа в выборке).
const BOOKS = [
  { name: 'BTC/USDT', vol: 4.75, half: 0.006 },
  { name: 'ETH/USDT', vol: 4.22, half: 0.022 },
  { name: 'SOL/USDT', vol: 4.74, half: 0.499 },
  { name: 'ETH/BTC',  vol: 4.55, half: 1.727 },
  { name: 'SOL/BTC',  vol: 15.24, half: 1.2 },
];
const GAMMA = 0.0047, ADV = 0.5, KAPPA = 1;
const K_BAND = 1.8, FLOOR = 2;   // правило проекта на стенде: z_lim = max(2, 1,8·f_м), z_mkt = max(z_lim, 1,8·f_т)

const fmt = (x, d = 1) => (Number.isFinite(x) ? x.toFixed(d) : '—');
const model = (b, fm, ft, sz, o = {}) => {
  const g = o.gamma ?? GAMMA, kap = o.kappa ?? KAPPA, a = o.adv ?? ADV, h = o.half ?? b.half;
  const P = { A: HE.riskA(g, b.vol), sz, theta: 0, kappa: kap, cm: 0.1 * (fm - h + a), ct: 0.1 * (ft + h), h: 0 };
  const r = HE.mtOptimize(P);
  const zlim = Math.max(FLOOR, K_BAND * fm), zmkt = Math.max(zlim, K_BAND * ft);
  const pj = HE.mtCost(Math.min(zlim, zmkt), zmkt, P);
  return { P, r, zlim, zmkt, Jm: r.J * 60, Jp: pj.J * 60, bm: r.makerZone ? r.bm : r.bt, bt: r.bt, zone: r.makerZone };
};

const out = [];
out.push('### Таблица 1. Пороги по книгам: модель и правило проекта (κ = 1, a = 0,5, γ = 0,0047)\n');
out.push('Пороги — тыс. USDT; затраты — USDT в час на одного агента; «правило» — действующее правило проекта (k = 1,8, пол 2). Шум потока ς_z — три сценария.\n');
for (const [fm, ft, label] of [[2, 10, 'Комиссии до стандарта: мейкер 2 bps, тейкер 10 bps'], [10, 10, 'Стандартная комиссия 0,1 %: мейкер 10 bps, тейкер 10 bps']]) {
  out.push('**' + label + '**\n');
  out.push('| Книга | ς, bps/√мин | s/2, bps | ς_z | Модель: мейкер / тейкер | Правило проекта | Затраты модели | Затраты правила | Правило дороже |');
  out.push('|---|---:|---:|---:|---|---|---:|---:|---:|');
  for (const b of BOOKS) for (const sz of [0.5, 2, 5]) {
    const m = model(b, fm, ft, sz);
    out.push(`| ${b.name} | ${fmt(b.vol, 2)} | ${fmt(b.half, 3)} | ${sz} | ${m.zone ? fmt(m.bm) + ' / ' + fmt(m.bt) : 'зоны мейкера нет: ' + fmt(m.bt)} | ${fmt(m.zlim)} / ${fmt(m.zmkt)} | ${fmt(m.Jm, 2)} | ${fmt(m.Jp, 2)} | ${fmt(100 * (m.Jp / m.Jm - 1), 0)} % |`);
  }
  out.push('');
}

out.push('### Таблица 2. Чувствительность порогов (книга типа SOL/USDT: ς = 4,74, s/2 = 0,5; комиссии 10/10; ς_z = 2; в строке меняется один параметр)\n');
out.push('| Параметр | Значение | Порог мейкера | Порог тейкера | Мейкер-зона |');
out.push('|---|---:|---:|---:|---|');
const bSens = BOOKS[2];   // SOL/USDT: полуспред 0,5 bps, у такой книги лимитная зона существует
const b0 = BOOKS[0];     // BTC/USDT-подобная книга — для таблицы дрейфа
for (const [nm, key, vals] of [['γ (неприятие риска)', 'gamma', [0.0012, 0.0047, 0.0188]], ['κ (скорость мейкера, тыс. USDT/мин)', 'kappa', [0.1, 1, 5]],
  ['a (неблагоприятный отбор, bps)', 'adv', [0, 0.5, 1.5]], ['s/2 (полуспред, bps)', 'half', [0.006, 0.5, 1.7]]]) {
  for (const v of vals) {
    const m = model(bSens, 10, 10, 2, { [key]: v });
    out.push(`| ${nm} | ${v} | ${m.zone ? fmt(m.bm, 2) : '—'} | ${fmt(m.bt, 2)} | ${m.zone ? 'есть' : 'нет'} |`);
  }
}
out.push('');

out.push('### Таблица 3. Устойчивый поток (дрейф) на книге BTC/USDT-подобной (комиссии 10/10, ς_z = 2): полоса только тейкера\n');
out.push('| Дрейф μ, тыс. USDT/мин | Число Пекле | Полоса тейкера | Партия при издержке заявки 0,5 USDT, тыс. USDT (раз в, мин) |');
out.push('|---|---:|---:|---|');
for (const mu of [0.01, 0.1, 0.5, 1, 3, 10]) {
  const A = HE.riskA(GAMMA, b0.vol), ct = 0.1 * (10 + b0.half);
  const d = HE.driftOptimize({ A, sz: 2, mu, ct });
  const bt = HE.batchOptimize({ A, mu, K: 0.5, h: 0 });
  out.push(`| ${mu} | ${fmt(d.Pe, 2)} | ${fmt(d.b, 2)} | ${fmt(bt.H, 2)} (${fmt(bt.interval, 1)}) |`);
}
out.push('');

out.push('### Таблица 4. Перевод актива между биржами (связка): порог по плате сети (ς_Δ = 0,7, ς_z = 2, капитал 10 % годовых)\n');
out.push('| Плата сети K, USDT | Порог перевода B | Цель b | Переводов в сутки | Затраты в сутки, USDT |');
out.push('|---:|---:|---:|---:|---:|');
for (const K of [0.1, 1, 2, 10, 50]) {
  const r = HE.trOptimize({ A: HE.riskA(GAMMA, 0.7), sz: 2, theta: 0, h: 1000 * 10 / 100 / 525600, K, ctr: 0, Td: 0 });
  out.push(`| ${K} | ${fmt(r.B, 1)} | ${fmt(r.b, 1)} | ${fmt(r.perHour * 24, 1)} | ${fmt(r.J * 60 * 24, 1)} |`);
}
console.log(out.join('\n'));
