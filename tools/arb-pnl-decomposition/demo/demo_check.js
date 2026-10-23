"use strict";
const { compute, solve } = require('./demo_core.js');
let fails = 0;
const chk = (name, ok, extra) => { console.log((ok ? '[OK]   ' : '[FAIL] ') + name + (extra ? '  ' + extra : '')); if (!ok) fails++; };

// ---------- 1) Обобщённый решатель той же системы BGB^T x = BGs (как в блоках 4-5 демо) ----------
// nodes: n, edges: {f,t,alpha,anchor}, pinned node index with potential xg
function clear(nNodes, edges, pin, xg) {
  const L = Array.from({ length: nNodes }, () => Array(nNodes).fill(0));
  const beta = Array(nNodes).fill(0);
  for (const e of edges) {
    L[e.f][e.f] += e.alpha; L[e.t][e.t] += e.alpha; L[e.f][e.t] -= e.alpha; L[e.t][e.f] -= e.alpha;
    beta[e.f] += e.alpha * e.anchor; beta[e.t] -= e.alpha * e.anchor;
  }
  const A = L.map(r => r.slice()), b = beta.slice();
  for (let u = 0; u < nNodes; u++) b[u] -= A[u][pin] * xg;
  for (let u = 0; u < nNodes; u++) { A[u][pin] = 0; A[pin][u] = 0; }
  A[pin][pin] = 1; b[pin] = xg;
  const x = solve(A, b);
  const w = edges.map(e => e.alpha * (e.anchor - (x[e.f] - x[e.t])));
  return { x, w };
}

// ---------- 2) Случай B из документа «Кривые котирования», §8.2 (сверка с напечатанными числами) ----------
// узлы: 0 USDT, 1 BTC@Binance, 2 BTC@OKX; рёбра: переводчик Binance (BTC@Bin/USDT, a=50), связка (BTC@Bin/BTC@OKX, a=100), переводчик OKX (a=50)
{
  const edges = [
    { f: 1, t: 0, alpha: 50, anchor: Math.log(60000) },
    { f: 1, t: 2, alpha: 100, anchor: 0 },
    { f: 2, t: 0, alpha: 50, anchor: Math.log(61200) },
  ];
  const r = clear(3, edges, 0, 0);
  // в документе знаки потока: переводчик Binance продаёт BTC (поток отрицательный по нашему знаку «покупка базы»)
  const J = Math.abs(r.w[1]);
  const S = r.w.map((w, i) => 1000 * w * w / (2 * edges[i].alpha));
  chk('Случай B: оборот J = 0.396 тыс. USDT', Math.abs(J - 0.396) < 0.0006, 'J=' + (1000 * J).toFixed(2) + ' USDT (док: 396,05)');
  chk('Случай B: обороты на всех трёх звеньях равны (круг)', Math.abs(Math.abs(r.w[0]) - J) < 1e-9 && Math.abs(Math.abs(r.w[2]) - J) < 1e-9);
  chk('Случай B: выгоды 1,57 / 0,78 / 1,57 USDT', Math.abs(S[0] - 1.57) < 0.01 && Math.abs(S[1] - 0.78) < 0.01 && Math.abs(S[2] - 1.57) < 0.01, S.map(v => v.toFixed(3)).join(' / '));
  chk('Случай B: сумма выгод = ε·J/2 = 3,92 USDT', Math.abs(S.reduce((a, b) => a + b, 0) - 3.92) < 0.01, 'sum=' + S.reduce((a, b) => a + b, 0).toFixed(3));
  const phiB = Math.exp(r.x[1]), phiO = Math.exp(r.x[2]);
  chk('Случай B: курсы фиксинга 60 477 и 60 717', Math.abs(phiB - 60477) < 2 && Math.abs(phiO - 60717) < 2, phiB.toFixed(0) + ' / ' + phiO.toFixed(0));
  // натуральная прибыль связки: получила BTC@Bin и отдала BTC@OKX
  const gainBTC = 1000 * (Math.abs(r.w[1]) / phiB - Math.abs(r.w[1]) / phiO);   // поток в тыс. USDT -> штуки BTC
  chk('Случай B: натуральная прибыль паритета связки ≈ 0,000026 BTC (≈1,57 USDT)', Math.abs(gainBTC - 0.000026) < 0.000002, 'gain=' + gainBTC.toFixed(6) + ' BTC = ' + (gainBTC * 60000).toFixed(2) + ' USDT');
}

// ---------- 3) Внутренняя согласованность демо (базовый сценарий) ----------
const base = { tau: .5, tstale: 500, ageB: 80, ageO: 190, quotes: [20.2, 15, 303, 20, 15.3, 306], D1: 100, D3: 250, D6: 500, theta: .6, WT: 10, riskT: .2, WA: 2, riskA: .02, VB: 20000, VO: 20000, fB: 1, fO: 1 };
const R = compute(base);
chk('Демо: неттинг по узлам Bw ≈ 0', R.nett < 1e-9, 'max|Bw|=' + R.nett.toExponential(1));
chk('Демо: ведомость в штуках закрывается по всем столбцам', Math.max(...R.colSum.map(Math.abs)) < 1e-12);
const sumRowsL = Math.max(...R.L.map(r => Math.abs(r.reduce((a, b) => a + b, 0))));
chk('Демо: суммы строк Λ = 0 (ранг 5)', sumRowsL < 1e-9);
// тождество выгоды: Σ S_i = ½ Σ w_i (ŝ_i − σ_i) (в k-USDT·nat → ×1000)
const lhs = R.surplusTot, rhs = 1000 * 0.5 * R.w.reduce((a, w, i) => a + w * (R.s[i] - R.sigma[i]), 0);
chk('Демо: Σ S_i = ½ Σ w_i·δ_i', Math.abs(lhs - rhs) < 1e-9, lhs.toFixed(4) + ' = ' + rhs.toFixed(4));
chk('Демо: все выгоды неотрицательны', R.surplus.every(v => v >= -1e-12));
// инвариантность к калибровке: сдвиг якоря-узла не меняет потоки
const x0 = R.x.slice();
const pinned = (g) => { const L = R.L, beta = R.beta; const A2 = L.map(r => r.slice()), b2 = beta.slice(); const gi = 5; for (let u = 0; u < 6; u++) b2[u] -= A2[u][gi] * g; for (let u = 0; u < 6; u++) { A2[u][gi] = 0; A2[gi][u] = 0; } A2[gi][gi] = 1; b2[gi] = g; return solve(A2, b2); };
const xs = pinned(Math.log(1000));
const w2 = R.alpha.map((a, i) => a * (R.s[i] - (xs[[0, 1, 0, 3, 4, 3, 0, 1, 2][i]] - xs[[1, 2, 2, 4, 5, 5, 3, 4, 5][i]])));
chk('Демо: потоки не зависят от калибровки узла (SOL@OKX = 200 или 1000)', Math.max(...w2.map((v, i) => Math.abs(v - R.w[i]))) < 1e-9);

// ---------- 4) Нулевое отклонение: нет межбиржевого разрыва -> нет потока (отсутствие арбитража) ----------
const eq = Object.assign({}, base, { quotes: [20, 15, 300, 20, 15, 300] });
const Re = compute(eq);
chk('Демо: при совпадающих котировках потоки связок = 0', Re.w.slice(6).every(v => Math.abs(v) < 1e-9), 'links=' + Re.w.slice(6).map(v => (1000 * v).toFixed(4)).join(','));

// ---------- 5) Вывод для базового сценария ----------
console.log('\nБазовый сценарий демо: потоки, у.е./такт и выгоды:');
const ids = ['BTC/ETH@Bin', 'ETH/SOL@Bin', 'BTC/SOL@Bin', 'BTC/ETH@OKX', 'ETH/SOL@OKX', 'BTC/SOL@OKX', 'связка BTC', 'связка ETH', 'связка SOL'];
ids.forEach((n, i) => console.log('  ' + n.padEnd(14) + ' w=' + (1000 * R.w[i]).toFixed(1).padStart(8) + '  S=' + R.surplus[i].toFixed(3).padStart(7)));
console.log('  Σ выгод = ' + R.surplusTot.toFixed(3) + ' у.е./такт; доля связок = ' + (100 * R.surplus.slice(6).reduce((a, b) => a + b, 0) / R.surplusTot).toFixed(1) + '%');
console.log(fails ? ('\nПровалено проверок: ' + fails) : '\nВсе проверки пройдены');
process.exit(fails ? 1 : 0);
