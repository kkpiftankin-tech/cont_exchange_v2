// Self-contained node-тест: `node ce_clearing_view.test.js`. Образцы JSON сгенерированы C++-кодом снимка клиринга
// (testdata/*.json) — тест фиксирует контракт matching → BFF и тексты пояснений «почему нет потока».
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { buildClearingView } = require('./ce_clearing_view');
let failed = 0;
const check = (n, c) => { if (c) console.log('[OK] ' + n); else { console.error('[FAIL] ' + n); failed++; } };
const load = (n) => JSON.parse(fs.readFileSync(path.join(__dirname, 'testdata', n), 'utf8'));

const admin = [
  { config: { venue_id: 'binance', is_active: true }, health: { status: 'connected', circuit_breaker_state: 'CLOSED' },
    metrics: { poll: { mode: 'scheduler', period_s: 3, paused: false, pairs: { 'BTC/USDT': { health: 0, age_s: 1.4, period_s: 3, fails: 0 }, 'ETH/USDT': { health: 0, age_s: 2.1, period_s: 3, fails: 0 } } } } },
  { config: { venue_id: 'okx', is_active: true }, health: { status: 'connected', circuit_breaker_state: 'CLOSED' }, metrics: { poll: { mode: 'scheduler', period_s: 3, paused: false, pairs: {} } } },
];

// --- Заблокированный арбитраж: потока нет, циклы закрыты
{
  const v = buildClearingView({ solve: load('ce_clearing_snapshot_blocked.json'), adminItems: admin, nowMs: 1792000001500 });
  check('снимок доступен', v.available && v.solve.converged);
  check('возраст решения = now − solved_at', v.solve.ageMs === 1000);
  check('уровень предупреждения: потока нет', v.explanation.level === 'warn' && /Потока нет/.test(v.explanation.headline));
  check('сказано, что все рёбра внутри полок и потенциалы равны нулю', /внутри своих полок/.test(v.explanation.headline) && /потенциалы узлов равны нулю/.test(v.explanation.headline));
  const texts = v.explanation.bullets.map((b) => b.text).join(' | ');
  check('назван заблокированный арбитраж и ближайший цикл', /арбитраж заблокирован/.test(texts) && /типовых циклов/.test(texts));
  check('показан состав порога цикла (age-полка, перевод)', /полка по возрасту данных ADR-075/.test(texts) && /издержки перевода/.test(texts));
  check('отброшенная котировка видна в пояснении', /не попали котировки/.test(texts) && /устарели/.test(texts));
  check('циклы обогащены составом порога', v.solve.cycles.every((c) => Number.isFinite(c.thr_age_pm) && Number.isFinite(c.thr_transfer_pm)));
  const c0 = v.solve.cycles[0];
  check('порог = age + spread + commission + transfer', Math.abs(c0.thr_age_pm + c0.thr_spread_pm + c0.thr_commission_pm + c0.thr_transfer_pm - c0.threshold_pm) < 1e-9);
  check('рёбра в б.п. (×10)', Math.abs(v.solve.edges[0].dead_zone_bp - v.solve.edges[0].dead_zone_pm * 10) < 1e-9);
  const bin = v.liquidity.venues.find((x) => x.venue_id === 'binance');
  const btc = bin.pairs.find((p) => p.symbol === 'BTC/USDT');
  check('ликвидность: пара в клиринге, состояние «внутри полки», возраст опроса', btc.in_clearing && /внутри полки/.test(btc.status) && btc.book_age_s === 1.4);
  check('ликвидность: возраст кривой в клиринге и σ переданы', btc.clearing_age_ms === 6000 && btc.sigma_pm === 0.1);
  check('состав полки переводчика (age) доступен', btc.dz_age_pm > 0 && btc.dz_spread_pm === 0);
  const kr = v.liquidity.venues.find((x) => x.venue_id === 'kraken');
  check('отброшенная пара показана у своей биржи с причиной', kr && kr.pairs.some((p) => p.symbol === 'SOL/USDT' && p.dropped_reason === 'stale' && /отброшена: устарела/.test(p.status)));
}

// --- Открытый арбитраж: поток есть
{
  const v = buildClearingView({ solve: load('ce_clearing_snapshot_open.json'), adminItems: admin, nowMs: 1792000001500 });
  check('поток есть: уровень ok', v.explanation.level === 'ok' && /Поток есть/.test(v.explanation.headline));
  check('открытые циклы упомянуты', v.explanation.bullets.some((b) => /Открытых типовых циклов: 2/.test(b.text)));
  check('активные рёбра в состоянии active', v.solve.edges.some((e) => e.state === 'active' && Math.abs(e.flow_kusdt) > 0));
}

// --- Нет снимка / решатель не сошёлся / биржа отключена
{
  const v = buildClearingView({ solve: null, adminItems: admin, solveError: 'http_404', nowMs: 1 });
  check('нет снимка: честно сказано, не нули', !v.available && v.solve === null && v.explanation.level === 'error' && /не прислал решение/.test(v.explanation.headline) && v.error === 'http_404');
  const s = load('ce_clearing_snapshot_blocked.json'); s.converged = false; s.iters = 100; s.max_imbalance = 0.5;
  const v2 = buildClearingView({ solve: s, adminItems: [{ config: { venue_id: 'okx', is_active: false }, health: {}, metrics: {} }], nowMs: 1792000001500 });
  check('не сошёлся: сообщение об ошибке решателя', v2.explanation.bullets.some((b) => b.level === 'error' && /не сошёлся/.test(b.text)));
  check('отключённая биржа названа в пояснении', v2.explanation.bullets.some((b) => /okx отключена оператором/.test(b.text)));
  const v3 = buildClearingView({ solve: load('ce_clearing_snapshot_blocked.json'), adminItems: [{ config: { venue_id: 'binance' }, health: { circuit_breaker_state: 'OPEN', circuit_breaker_reason: 'snapshot_request_failed' }, metrics: {} }], nowMs: 1792000001500 });
  check('открытый предохранитель назван в пояснении', v3.explanation.bullets.some((b) => /предохранитель OPEN/.test(b.text)));
}
// --- Устаревший снимок: новые такты не приходят ⇒ прежний вывод «потока нет» не выдаётся за текущий
{
  const v = buildClearingView({ solve: load('ce_clearing_snapshot_blocked.json'), adminItems: admin, nowMs: 1792000001500 + 120000 });
  check('устаревший снимок: уровень error, в заголовке «устарело»', v.explanation.level === 'error' && /устарело/.test(v.explanation.headline) && !/^Потока нет/.test(v.explanation.headline));
  check('устаревший снимок: прежний вывод сохранён пунктом «последнее известное решение»', v.explanation.bullets.some((b) => /Последнее известное решение/.test(b.text) && /Потока нет/.test(b.text)));
}

// --- Активные арбитражёры при 0 открытых типовых циклов: не утверждаем «поток только внутри бирж» и «арбитраж заблокирован»
{
  const s = load('ce_clearing_snapshot_open.json');
  s.summary.cycles_open = 0; s.summary.active_arbitrageurs = 2; s.summary.active = 3;
  s.cycles = s.cycles.map((c) => ({ ...c, state: 'blocked', margin_pm: 0.1 }));
  const v = buildClearingView({ solve: s, adminItems: admin, nowMs: 1792000001500 });
  const t = v.explanation.bullets.map((b) => b.text).join(' | ');
  check('активные арбитражёры: нет ложного «внутри бирж»', !/поток идёт внутри бирж/.test(t) && /арбитражёры активны/.test(t));
  check('активные арбитражёры: нет ложного «арбитраж заблокирован»', !/арбитраж заблокирован/.test(t));
}

// --- Рёбра без глубины и ненулевые потенциалы: заголовок не врёт про «все внутри полок» и «потенциалы равны нулю»
{
  const s = load('ce_clearing_snapshot_blocked.json');
  s.summary.no_depth = 2; s.summary.in_dead_zone = s.summary.edges - 2;
  s.nodes[0].potential_pm = 0.7;
  const v = buildClearingView({ solve: s, adminItems: admin, nowMs: 1792000001500 });
  check('без глубины названо отдельно', /2 без глубины/.test(v.explanation.headline) && !/потенциалы узлов равны нулю/.test(v.explanation.headline));
  s.cycles[0].state = 'no_depth';
  const v2 = buildClearingView({ solve: s, adminItems: admin, nowMs: 1792000001500 });
  check('циклы без глубины названы', v2.explanation.bullets.some((b) => /не могут открыться: у одного из четырёх рёбер нет глубины/.test(b.text)));
}

// --- Пустой объект снимка (нет рёбер)
{
  const v = buildClearingView({ solve: {}, adminItems: admin, nowMs: 1 });
  check('пустой снимок: честное сообщение, без «Потока нет»', v.explanation.level === 'error' && /Снимок решения пуст/.test(v.explanation.headline));
}
assert.strictEqual(failed, 0, failed + ' failed');
console.log('ce_clearing_view tests passed');
