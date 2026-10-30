"use strict";
// Простейший случай «две биржи, один актив» (случай B документа «Кривые котирования», §8.2):
// BTC = 60 000 на Binance (A) и 61 200 на OKX (B); без комиссий и без проскальзывания внешних заявок.
// Считаем РЕАЛЬНЫЙ денежный результат каждого участника при трёх способах закрыть позицию связки.
const { solve } = require('./demo_core.js');
function clear(nNodes, edges, pin, xg) {
  const L = Array.from({ length: nNodes }, () => Array(nNodes).fill(0)); const beta = Array(nNodes).fill(0);
  for (const e of edges) { L[e.f][e.f] += e.alpha; L[e.t][e.t] += e.alpha; L[e.f][e.t] -= e.alpha; L[e.t][e.f] -= e.alpha; beta[e.f] += e.alpha * e.anchor; beta[e.t] -= e.alpha * e.anchor; }
  const A = L.map(r => r.slice()), b = beta.slice();
  for (let u = 0; u < nNodes; u++) b[u] -= A[u][pin] * xg;
  for (let u = 0; u < nNodes; u++) { A[u][pin] = 0; A[pin][u] = 0; } A[pin][pin] = 1; b[pin] = xg;
  const x = solve(A, b); return { x, w: edges.map(e => e.alpha * (e.anchor - (x[e.f] - x[e.t]))) };
}
const mA = 60000, mB = 61200;
// узлы: 0 USDT, 1 BTC@A, 2 BTC@B; рёбра: переводчик A, связка (A->B), переводчик B
const edges = [{ f: 1, t: 0, alpha: 50, anchor: Math.log(mA) }, { f: 1, t: 2, alpha: 100, anchor: 0 }, { f: 2, t: 0, alpha: 50, anchor: Math.log(mB) }];
const r = clear(3, edges, 0, 0);
const phiA = Math.exp(r.x[1]), phiB = Math.exp(r.x[2]);
const J = 1000 * Math.abs(r.w[1]);                 // USDT за такт, один и тот же на всех звеньях
const qA = J / phiA, qB = J / phiB;                // штуки BTC, которые прошли через связку на узлах A и B
console.log('Клиринг: курс фиксинга A = ' + phiA.toFixed(0) + ', B = ' + phiB.toFixed(0) + '; оборот по кругу J = ' + J.toFixed(2) + ' USDT за такт на каждом звене');
console.log('Штуки BTC: связка получает ' + qA.toFixed(6) + ' на A и отдаёт ' + qB.toFixed(6) + ' на B; остаток ' + (qA - qB).toFixed(6) + ' BTC');
// Переводчик A продал qA BTC за J USDT (по фиксингу), переводчик B купил qB BTC за J USDT.
// Закрытие внешними заявками по середине (без проскальзывания и комиссии):
const tA = J - qA * mA;            // переводчик A: получил J, купил qA на A по mA
const tB = qB * mB - J;            // переводчик B: заплатил J, продал qB на B по mB
console.log('\nПереводчики (внутренняя сделка + закрытие на своей бирже по середине): A = ' + tA.toFixed(2) + ' USDT, B = ' + tB.toFixed(2) + ' USDT');
// Связка: получила qA BTC на A за J USDT (внутри), отдала qB BTC на B за J USDT.
const carrierTransfer = (qA - qB) * mB;                       // физический перевод 1:1, остаток по цене B
const carrierMarketCorrect = qA * mA - qB * mB;               // закрытие позиции рынком в «правильную» сторону: продала qA на A, купила qB на B
const carrierMarketInverted = -(qA * mA) + qB * mB;           // как в реализации: покупает на A, продаёт на B (знаки ног наоборот)
console.log('\nСвязка, три способа закрыть позицию:');
console.log('  1) физический перевод BTC с A на B (теория):               ' + carrierTransfer.toFixed(2) + ' USDT   (остаток в штуках ' + (qA - qB).toFixed(6) + ' BTC)');
console.log('  2) рыночное закрытие обеих ног «в свою сторону» (отменяет хеджи переводчиков): ' + carrierMarketCorrect.toFixed(2) + ' USDT');
console.log('  3) рыночное закрытие с перевёрнутыми знаками (как в ce_position_projection.hpp): ' + carrierMarketInverted.toFixed(2) + ' USDT, но ' + (2 * qA).toFixed(6) + ' BTC покупается на A и столько же продаётся на B — удвоение оборота и перекос запасов');
console.log('\nИтого по системе (сумма участников):');
console.log('  теория (перевод):              ' + (tA + tB + carrierTransfer).toFixed(2) + ' USDT');
console.log('  рыночное закрытие правильное:  ' + (tA + tB + carrierMarketCorrect).toFixed(2) + ' USDT   <- прибыль арбитража уничтожена');
console.log('  рыночное закрытие перевёрнутое:' + (tA + tB + carrierMarketInverted).toFixed(2) + ' USDT   <- вдвое больше оборота; запас BTC копится на A и тает на B (' + (2 * qA).toFixed(6) + ' BTC за такт)');
