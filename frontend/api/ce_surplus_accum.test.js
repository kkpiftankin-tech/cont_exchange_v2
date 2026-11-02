// Self-contained node-тест: `node ce_surplus_accum.test.js` (у frontend/api нет jest).
const { createAccumulator, accumulate, drain, restore } = require('./ce_surplus_accum');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}
const near = (a, b, eps = 1e-12) => Math.abs(a - b) < eps;

// Форма СЫРОГО снимка matching (GET /ce/clearing/last): snake_case, рёбра с gap_pm = |d| − dead_zone_pm.
const snap = (batchId, ms, edges) => ({ batch_id: batchId, event_time_ms: ms, edges });
const edge = (name, flow, dev, dz = 0) => ({ name, kind: 'translator', flow_kusdt: flow, deviation_pm: dev, dead_zone_pm: dz, gap_pm: Math.abs(dev) - dz });

// --- формула и форма снимка
let acc = createAccumulator();
check('сырой снимок (batch_id, event_time_ms) учитывается', accumulate(acc, snap('w|1000', 1000, [edge('A', 3, 0.1), edge('B', -2, -0.05)])) === true);
check('повтор того же такта не учитывается', accumulate(acc, snap('w|1000', 1000, [edge('A', 3, 0.1)])) === false);
let d = drain(acc);
check('S(A) = ½·3·0,1 = 0,15 USDT', near(d.agents.get('A').surplus, 0.15));
check('S(B) = ½·2·0,05 = 0,05 USDT (знаки не влияют)', near(d.agents.get('B').surplus, 0.05));
check('сумма |потоков| и число тактов', near(d.agents.get('A').absFlow, 3) && d.agents.get('A').tacts === 1 && d.tacts === 1);
check('выгода неотрицательна при любых знаках', [...d.agents.values()].every((a) => a.surplus >= 0));
acc = createAccumulator();
check('вид BFF (batchId, eventTimeMs) тоже принимается', accumulate(acc, { batchId: 'w|7', eventTimeMs: 7, edges: [{ name: 'A', flow_kusdt: 1, deviation_pm: 0.2 }] }) === true && near(drain(acc).agents.get('A').surplus, 0.1));

// --- мёртвая зона: выгода считается от отклонения за вычетом зоны (поток = depth·(|d| − c))
acc = createAccumulator();
accumulate(acc, snap('w|1', 1, [edge('Z', 2, 0.5, 0.3), edge('IN', 0, 0.2, 0.3)]));
d = drain(acc);
check('с мёртвой зоной S = ½·|f|·(|d|−c) = ½·2·0,2 = 0,2', near(d.agents.get('Z').surplus, 0.2));
check('внутри мёртвой зоны выгода 0', near(d.agents.get('IN').surplus, 0));

// --- случай B теории: 396,05 USDT оборота, глубины 50/100/50 (тыс. USDT на единицу логарифма) ⇒ выгоды 1,57/0,78/1,57
// в примерах теории отклонение в ‰ равно 1000·J/α
const J = 0.39605;   // тыс. USDT
const acc2 = createAccumulator();
accumulate(acc2, snap('w|1', 1, [edge('Tbin', -J, 1000 * J / 50), edge('Link', J, 1000 * J / 100), edge('Tokx', J, 1000 * J / 50)]));
const d2 = drain(acc2);
check('случай B: выгода переводчика ≈ 1,57 USDT', Math.abs(d2.agents.get('Tbin').surplus - 1.57) < 0.01);
check('случай B: выгода связки ≈ 0,78 USDT', Math.abs(d2.agents.get('Link').surplus - 0.78) < 0.01);
check('случай B: сумма ≈ 3,92 USDT', Math.abs([...d2.agents.values()].reduce((s, a) => s + a.surplus, 0) - 3.92) < 0.01);

// --- накопление по нескольким тактам и сброс копилки
acc = createAccumulator();
for (let i = 0; i < 5; i++) accumulate(acc, snap('w|' + (i + 1), 1000 + 1000 * i, [edge('A', 2, 0.1)]));
d = drain(acc);
check('5 тактов суммируются', near(d.agents.get('A').surplus, 5 * 0.1) && d.tacts === 5);
check('при полном ряду ожидаемое число тактов = учтённому', d.expected === 5);
check('после drain копилка пуста, но последний такт помнится', drain(acc).tacts === 0 && accumulate(acc, snap('w|5', 5000, [edge('A', 2, 0.1)])) === false);

// --- пропуски внутри интервала: шаг 1000 мс, учтены 1,2,3,6,7 (4 и 5 пропущены) ⇒ ожидаемо 7
acc = createAccumulator();
[1, 2, 3, 6, 7].forEach((i) => accumulate(acc, snap('w|' + i, i * 1000, [edge('A', 1, 0.1)])));
d = drain(acc);
check('пропуски внутри интервала видны: учтено 5 из 7', d.tacts === 5 && d.expected === 7);

// --- пропуск в НАЧАЛЕ следующего интервала (опрос молчал) виден благодаря границе прошлого drain
acc = createAccumulator();
for (let i = 1; i <= 10; i++) accumulate(acc, snap('w|' + i, i * 1000, [edge('A', 1, 0.1)]));
drain(acc);
for (let i = 15; i <= 20; i++) accumulate(acc, snap('w|' + i, i * 1000, [edge('A', 1, 0.1)]));   // такты 11…14 пропущены
d = drain(acc);
check('провал в начале интервала учтён: 6 учтённых из 10 ожидаемых', d.tacts === 6 && d.expected === 10);

// --- возврат сброшенного при сбое записи
acc = createAccumulator();
for (let i = 1; i <= 4; i++) accumulate(acc, snap('w|' + i, i * 1000, [edge('A', 2, 0.1)]));
const lost = drain(acc);
for (let i = 5; i <= 6; i++) accumulate(acc, snap('w|' + i, i * 1000, [edge('A', 2, 0.1), edge('N', 1, 0.2)]));
restore(acc, lost);
d = drain(acc);
check('restore: суммы и такты складываются (6 тактов, S(A)=0,6)', d.tacts === 6 && near(d.agents.get('A').surplus, 0.6) && near(d.agents.get('N').surplus, 0.2));
check('restore: ожидаемое число тактов считается от прежней границы (6)', d.expected === 6);
restore(acc, null);
check('restore(null) безопасен', true);

// --- устойчивость к мусору
acc = createAccumulator();
accumulate(acc, snap('w|9', 9000, [{ name: 'A', flow_kusdt: null, deviation_pm: 0.1 }, { name: 'B', flow_kusdt: 1 }, { name: '', flow_kusdt: 1, deviation_pm: 1 }, null, { name: 'C', flow_kusdt: 'x', deviation_pm: 1 }, edge('D', 1, 0.2)]));
d = drain(acc);
check('пустые/битые поля пропускаются, остальные считаются', d.agents.size === 1 && d.agents.has('D') && near(d.agents.get('D').surplus, 0.1));
check('нет снимка / нет batch_id → false', accumulate(createAccumulator(), null) === false && accumulate(createAccumulator(), { edges: [] }) === false);

if (failed) { console.error(failed + ' check(s) failed'); process.exit(1); }
console.log('all passed');
