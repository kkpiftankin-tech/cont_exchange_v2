// Расчётное ядро порогов хеджа HE — дословная выборка из incoming-docs/2026-10-29-porogi-hedzha5-380dc48d.html
// (sha1-8 380dc48d). Не править: перегенерируется extract_engine.py. Единицы: позиция z — тыс. USDT,
// время — минуты, деньги — USDT; cm, ct, ctr — USDT на 1 тыс. USDT оборота (= 0,1 × bps); K — USDT.
var HE = (function () {
  'use strict';
  function riskA(gamma, sigmaBps) { var s = sigmaBps * 1e-4; return 0.5 * gamma * s * s * 1e6; }
  function cubeRootBand(c, sz, A) { return Math.cbrt(3 * c * sz * sz / (4 * A)); }
  function quarterTrigger(K, sz, A) { return Math.pow(6 * K * sz * sz / A, 0.25); }

  /* моменты плотности w(z)=exp(−(θz²+2κ(z−bm)₊)/σ²) на [a,b] по Симпсону за один проход */
  function moments(a, b, n, w, acc) {
    if (!(b > a)) return;
    if (n % 2) n++;
    var hh = (b - a) / n;
    for (var i = 0; i <= n; i++) {
      var z = a + i * hh, wz = w(z), k = (i === 0 || i === n) ? 1 : (i % 2 ? 4 : 2), q = k * wz * hh / 3;
      acc.Z0 += q; acc.M1 += q * z; acc.M2 += q * z * z;
      if (acc.tail && z >= acc.from) acc.T += q;
    }
  }
  function mtCost(bm, bt, P) {
    var th = P.theta || 0, sz2 = P.sz * P.sz, ka = P.kappa || 0, h = P.h || 0;
    var w = function (z) { return Math.exp(-(th * z * z + 2 * ka * Math.max(0, z - bm)) / sz2); };
    // отсечение хвоста: дальше показатель > 40, вес < 4·10⁻¹⁸
    var zc = bt;
    if (th > 0) zc = Math.min(zc, Math.sqrt(40 * sz2 / th));
    if (ka > 0 && bm < bt) zc = Math.min(zc, bm + 20 * sz2 / ka);
    var lo = Math.min(bm, zc), acc = { Z0: 0, M1: 0, M2: 0, T: 0, tail: false, from: 0 };
    moments(0, lo, 120, w, acc);
    var acc2 = { Z0: 0, M1: 0, M2: 0, T: 0, tail: true, from: -1 };
    moments(lo, zc, 160, w, acc2);
    var Z0 = acc.Z0 + acc2.Z0, M1 = acc.M1 + acc2.M1, M2 = acc.M2 + acc2.M2;
    var T = bm < bt ? acc2.Z0 : 0;
    var Ez2 = M2 / Z0, Eabs = M1 / Z0, pM = ka > 0 ? T / Z0 : 0, pb = w(bt) / (2 * Z0);
    var risk = P.A * Ez2 + h * Eabs, makerVol = ka * pM, takerVol = sz2 * pb;
    var maker = P.cm * makerVol, taker = P.ct * takerVol;
    return { J: risk + maker + taker, risk: risk, maker: maker, taker: taker, makerVol: makerVol, takerVol: takerVol, pMaker: pM, Ez2: Ez2, Eabs: Eabs, pb: pb };
  }
  function golden(f, a, b, it) {
    var g = (Math.sqrt(5) - 1) / 2, x1 = b - g * (b - a), x2 = a + g * (b - a), f1 = f(x1), f2 = f(x2);
    for (var i = 0; i < (it || 30); i++) {
      if (f1 < f2) { b = x2; x2 = x1; f2 = f1; x1 = b - g * (b - a); f1 = f(x1); }
      else { a = x1; x1 = x2; f1 = f2; x2 = a + g * (b - a); f2 = f(x2); }
    }
    return f1 < f2 ? { x: x1, f: f1 } : { x: x2, f: f2 };
  }
  function argmin(f, a, b, n, log, it) {
    var best = { f: Infinity, i: 0 }, xs = [];
    for (var i = 0; i <= n; i++) {
      var t = i / n, x = log ? Math.exp(Math.log(a) + t * (Math.log(b) - Math.log(a))) : a + t * (b - a), v = f(x);
      xs.push(x);
      if (v < best.f) best = { x: x, f: v, i: i };
    }
    var r = golden(f, xs[Math.max(0, best.i - 1)], xs[Math.min(n, best.i + 1)], it || 24);
    return r.f <= best.f ? r : best;
  }
  /* корень сглаженного склеивания: A·b² + (h − θ·ct)·b = ρ + κ·(ct − cm) */
  function smoothFitBt(P, rho, withMaker) {
    var a = P.A, bb = (P.h || 0) - (P.theta || 0) * P.ct, c = -(rho + (withMaker ? (P.kappa || 0) * (P.ct - P.cm) : 0));
    return (-bb + Math.sqrt(bb * bb - 4 * a * c)) / (2 * a);
  }
  /* Уравнение Беллмана для V'(z) = W: W' = a(z)·W + f(z); интегрируем НАЗАД от b_t (W = ct) экспоненциальной схемой
     (устойчиво при любом шаге). При makerOn в зоне мейкера a = 2(θz+κ)/σ², f = 2(ρ − g − κ·cm)/σ²; переход в зону
     бездействия — там, где W опускается до cm: это и есть порог мейкера b_m. */
  function hjbBack(P, rho, bt, makerOn) {
    var th = P.theta || 0, sz2 = P.sz * P.sz, ka = makerOn ? (P.kappa || 0) : 0, h = P.h || 0;
    var amax = (2 / sz2) * (th * bt + ka);
    var n = Math.min(40000, Math.max(1200, Math.ceil(6 * amax * bt)));
    var dz = bt / n, W = P.ct, inMaker = makerOn, bm = makerOn ? null : bt, pts = [[bt, W]], every = Math.max(1, Math.round(n / 2000));
    for (var i = n; i > 0; i--) {
      var z0 = i * dz, zm = z0 - dz / 2, g = P.A * zm * zm + h * zm;
      var a = (2 / sz2) * (th * zm + (inMaker ? ka : 0)), f = (2 / sz2) * (rho - g - (inMaker ? ka * P.cm : 0));
      var Wn;
      if (a * dz < 1e-9) Wn = W - dz * (a * W + f);
      else { var E = Math.exp(-a * dz); Wn = W * E - (f / a) * (1 - E); }
      if (inMaker && Wn <= P.cm) {               // пересечение уровня cm внутри шага: линейная интерполяция
        var t = (W - P.cm) / (W - Wn);
        bm = z0 - t * dz; inMaker = false;
      }
      W = Wn;
      if ((n - i) % every === 0 || i === 1) pts.push([z0 - dz, W]);
    }
    if (inMaker) bm = 0;                           // уровень cm так и не достигнут: мейкер выгоден всюду (cm ≤ 0)
    return { pts: pts.reverse(), bm: bm, W0: W };
  }
  function mtOptimize(P) {
    var b0 = cubeRootBand(P.ct, P.sz, P.A), th = P.theta || 0;
    var blin = th > 0 ? P.ct * th / P.A : 0;
    var U = Math.max(25 * b0, 3 * blin);
    var useMaker = (P.kappa || 0) > 0 && P.cm < P.ct;
    var inner = function (bt) {
      if (!useMaker) return { x: 1, f: mtCost(bt, bt, P).J };
      return argmin(function (r) { return mtCost(r * bt, bt, P).J; }, 0, 1, 8, false, 22);
    };
    var outer = argmin(function (bt) { return inner(bt).f; }, b0 * 0.02, U, 14, true, 26);
    var rho = outer.f, btS = outer.x, bmS = useMaker ? inner(btS).x * btS : btS;
    // пороги из условий оптимальности уравнения Беллмана при найденной средней стоимости ρ
    var makerOn = useMaker && bmS < btS * 0.999;
    var bt = smoothFitBt(P, rho, makerOn);
    if (!(bt > 0) || !isFinite(bt)) bt = btS;
    var back = hjbBack(P, rho, bt, makerOn);
    var bm = back.bm;
    if (makerOn && !(bm < bt)) { makerOn = false; bm = bt; }
    var res = mtCost(bm, bt, P);
    res.bm = bm; res.bt = bt; res.rho = rho; res.b0 = b0; res.blin = blin; res.makerZone = makerOn;
    res.btSearch = btS; res.bmSearch = bmS; res.vp = back.pts; res.W0 = back.W0;
    return res;
  }
  function vprime(P, sol) { return sol.vp; }
  /* Перевод: при |z| = B — перевод до |z| = b. Цикл стоит K + ctr·(B−b) + (g(B)−g(b))·Td. */
  function trCost(b, B, P) {
    var th = P.theta || 0, sz2 = P.sz * P.sz, h = P.h || 0, n = 240, dy = B / n;
    var g = function (u) { return P.A * u * u + h * u; };
    var I1 = 0, Ig = 0, ys = new Float64Array(n + 1), i1 = new Float64Array(n + 1), ig = new Float64Array(n + 1);
    for (var k = 0; k < n; k++) {
      var y0 = k * dy, y1 = y0 + dy, m = th > 0 ? Math.exp(th * (y1 * y1 - y0 * y0) / sz2) : 1;
      I1 = I1 * m + 0.5 * dy * (m + 1);
      Ig = Ig * m + 0.5 * dy * (g(y0) * m + g(y1));
      ys[k + 1] = y1; i1[k + 1] = I1; ig[k + 1] = Ig;
    }
    var T = 0, R = 0, kb = Math.min(n - 1, Math.floor(b / dy));
    for (var j = kb; j < n; j++) {
      var a0 = Math.max(ys[j], b), a1 = ys[j + 1];
      if (a1 <= a0) continue;
      var f = (a0 - ys[j]) / dy, I1a = i1[j] + f * (i1[j + 1] - i1[j]), Iga = ig[j] + f * (ig[j + 1] - ig[j]);
      T += 0.5 * (a1 - a0) * (I1a + i1[j + 1]); R += 0.5 * (a1 - a0) * (Iga + ig[j + 1]);
    }
    T *= 2 / sz2; R *= 2 / sz2;
    var fee = P.K + P.ctr * (B - b), delay = (g(B) - g(b)) * (P.Td || 0);
    return { J: (fee + delay + R) / T, T: T, risk: R / T, fee: fee / T, delay: delay / T, perHour: 60 / T, size: B - b };
  }
  function trOptimize(P) {
    var B0 = P.K > 0 ? quarterTrigger(P.K, P.sz, P.A) : (P.ctr > 0 ? cubeRootBand(P.ctr, P.sz, P.A) : 1);
    if (P.h > 0 && P.K > 0) B0 = Math.min(B0, Math.cbrt(6 * P.K * P.sz * P.sz / P.h));
    var U = 30 * B0;
    if ((P.theta || 0) > 0) U = Math.min(U, Math.sqrt(400 * P.sz * P.sz / P.theta));
    var inner = function (B) { return argmin(function (r) { return trCost(r * B, B, P).J; }, 0, 0.999, 8, false, 22); };
    var outer = argmin(function (B) { return inner(B).f; }, B0 * 0.01, U, 14, true, 26);
    var B = outer.x, r = inner(B), res = trCost(r.x * B, B, P);
    res.B = B; res.b = r.x * B; res.B0 = B0; res.atCap = B > 0.95 * U;
    return res;
  }
  /* Устойчивый поток (дрейф μ, тыс. USDT/мин) в полосе [−b, b] с отражением.
     Плотность ∝ exp(2μz/ς_z²); y = 2μb/ς_z² (число Пекле полосы).
     E[z²] = b²·(1 − (2/y)·coth y + 2/y²); объём хеджа на двух границах μ·coth y (при μ → 0 — ς_z²/(2b)). */
  function cothSafe(y) { if (y > 20) return 1; var e = Math.exp(-2 * y); return (1 + e) / (1 - e); }
  function driftShape(y) { // множитель при b² в E[z²]
    if (y < 0.05) return 1 / 3 + 2 * y * y / 45 - 4 * Math.pow(y, 4) / 945;
    return 1 - (2 / y) * cothSafe(y) + 2 / (y * y);
  }
  function driftCost(b, P) {
    var sz2 = P.sz * P.sz, mu = Math.abs(P.mu || 0), y = 2 * mu * b / sz2;
    var Ez2 = b * b * driftShape(y), vol = y < 1e-6 ? sz2 / (2 * b) : mu * cothSafe(y);
    var risk = P.A * Ez2, fee = P.ct * vol;
    return { J: risk + fee, risk: risk, fee: fee, vol: vol, Ez2: Ez2, Pe: y };
  }
  function driftOptimize(P) {
    var b0 = cubeRootBand(P.ct, P.sz, P.A);
    var r = argmin(function (b) { return driftCost(b, P).J; }, b0 * 1e-4, b0 * 3, 40, true, 30);
    var res = driftCost(r.x, P); res.b = r.x; res.b0 = b0;
    return res;
  }
  /* Партия при фиксированной издержке заявки K (USDT) и устойчивом потоке μ: пила 0 → H.
     J(H) = K·μ/H + A·H²/3 + h·H/2 (риск — дисперсия, капитал — линейно). */
  function batchCost(H, P) {
    var mu = Math.abs(P.mu || 0), fix = P.K * mu / H, risk = P.A * H * H / 3, cap = (P.h || 0) * H / 2;
    return { J: fix + risk + cap, fix: fix, risk: risk, cap: cap, interval: mu > 0 ? H / mu : Infinity };
  }
  function batchOptimize(P) {
    var mu = Math.abs(P.mu || 0);
    if (!(mu > 0) || !(P.K > 0)) return { H: NaN, J: 0, fix: 0, risk: 0, cap: 0, interval: Infinity };
    var Hc = Math.cbrt(3 * P.K * mu / (2 * P.A)), Hl = (P.h || 0) > 0 ? Math.sqrt(2 * P.K * mu / P.h) : Infinity;
    var lo = Math.min(Hc, Hl) * 0.05, hi = Math.min(Hc, Hl) * 3;
    var r = argmin(function (H) { return batchCost(H, P).J; }, lo, hi, 30, true, 30);
    var res = batchCost(r.x, P); res.H = r.x; res.Hcube = Hc; res.Hlin = Hl;
    return res;
  }
  /* Монте-Карло для мейкер/тейкер: средние затраты и путь */
  function rng(seed) { var s = seed >>> 0 || 1; return function () { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; }; }
  function gauss(r) { var u = 0, v = 0; while (u === 0) u = r(); v = r(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); }
  function simulateMT(P, sol, minutes, dt, seed, record, clip) {
    var r = rng(seed), z = 0, t = 0, steps = Math.round(minutes / dt), sq = Math.sqrt(dt), th = P.theta || 0, h = P.h || 0;
    var risk = 0, mk = 0, tk = 0, path = [], ev = [], every = Math.max(1, Math.round(steps / 1200));
    var ka = sol.makerZone ? P.kappa : 0, lam = ka > 0 ? ka / clip : 0;
    for (var i = 0; i < steps; i++) {
      z += -th * z * dt + P.sz * sq * gauss(r);
      if (ka > 0 && Math.abs(z) > sol.bm && r() < lam * dt) {
        var q = Math.min(clip, Math.abs(z)); z -= Math.sign(z) * q; mk += P.cm * q; if (record) ev.push([t, z, 'm']);
      }
      if (Math.abs(z) > sol.bt) { var x = Math.abs(z) - sol.bt; tk += P.ct * x; z = Math.sign(z) * sol.bt; if (record) ev.push([t, z, 't', x]); }
      risk += (P.A * z * z + h * Math.abs(z)) * dt; t += dt;
      if (record && i % every === 0) path.push([t, z]);
    }
    return { J: (risk + mk + tk) / minutes, risk: risk / minutes, maker: mk / minutes, taker: tk / minutes, path: path, ev: ev };
  }
  function simulateTR(P, sol, minutes, dt, seed, record) {
    var r = rng(seed), z = 0, t = 0, steps = Math.round(minutes / dt), sq = Math.sqrt(dt), th = P.theta || 0, h = P.h || 0;
    var risk = 0, fee = 0, path = [], ev = [], every = Math.max(1, Math.round(steps / 1200));
    for (var i = 0; i < steps; i++) {
      z += -th * z * dt + P.sz * sq * gauss(r);
      if (Math.abs(z) >= sol.B) { var x = Math.abs(z) - sol.b; fee += P.K + P.ctr * x; if (record) ev.push([t, z, x]); z = Math.sign(z) * sol.b; }
      risk += (P.A * z * z + h * Math.abs(z)) * dt; t += dt;
      if (record && i % every === 0) path.push([t, z]);
    }
    return { J: (risk + fee) / minutes, risk: risk / minutes, fee: fee / minutes, path: path, ev: ev };
  }
  /* Независимая проверка: марковская цепь + относительная итерация по ценности (асинхронно, порциями) */
  function rviMT(P, Z, n, iters) {
    var dz = Z / n, N = 2 * n + 1, dt = dz * dz / (P.sz * P.sz), V = new Float64Array(N), C = new Float64Array(N), Wv = new Float64Array(N), pol = new Int8Array(N);
    var pf = (P.kappa || 0) * dt / dz, th = P.theta || 0, h = P.h || 0, rho = 0, it = 0;
    function step(k) {
      for (var s = 0; s < k && it < iters; s++, it++) {
        for (var i = 0; i < N; i++) {
          var zi = (i - n) * dz, run = (P.A * zi * zi + h * Math.abs(zi)) * dt, dn = Math.max(0, i - 1), up = Math.min(N - 1, i + 1);
          // за краем сетки — принудительный тейкер-хедж (а не бесплатное отражение, которое исказило бы порог)
          var Vu = i + 1 < N ? V[up] : V[i] + P.ct * dz, Vd = i > 0 ? V[dn] : V[i] + P.ct * dz;
          var q = Math.min(0.5, Math.max(-0.5, -th * zi * dt / (2 * dz))), EV = (0.5 + q) * Vu + (0.5 - q) * Vd, best = run + EV, a = 0;
          if (pf > 0 && i !== n) {
            var tw = zi > 0 ? i - 1 : i + 1, EVm = (0.5 + q) * V[Math.min(N - 1, tw + 1)] + (0.5 - q) * V[Math.max(0, tw - 1)];
            var m = run + pf * (P.cm * dz + EVm) + (1 - pf) * EV;
            if (m < best) { best = m; a = 1; }
          }
          C[i] = best; pol[i] = a;
        }
        for (i = 0; i < N; i++) Wv[i] = C[i];
        for (i = n + 1; i < N; i++) { var v = Wv[i - 1] + P.ct * dz; if (v < Wv[i]) { Wv[i] = v; pol[i] = 2; } }
        for (i = n - 1; i >= 0; i--) { v = Wv[i + 1] + P.ct * dz; if (v < Wv[i]) { Wv[i] = v; pol[i] = 2; } }
        rho = (Wv[n] - V[n]) / dt;
        var base = Wv[n];
        for (i = 0; i < N; i++) V[i] = Wv[i] - base;
      }
      var bm = null, bt = null;
      for (i = n; i < N; i++) { if (bm === null && pol[i] === 1) bm = (i - n) * dz; if (bt === null && pol[i] === 2) bt = (i - n - 1) * dz; }
      return { done: it >= iters, progress: it / iters, rho: rho, bm: bm, bt: bt };
    }
    return step;
  }
  return { riskA: riskA, cubeRootBand: cubeRootBand, quarterTrigger: quarterTrigger, mtCost: mtCost, mtOptimize: mtOptimize,
           vprime: vprime, trCost: trCost, trOptimize: trOptimize, simulateMT: simulateMT, simulateTR: simulateTR, rviMT: rviMT, smoothFitBt: smoothFitBt,
           cothSafe: cothSafe, driftShape: driftShape, driftCost: driftCost, driftOptimize: driftOptimize, batchCost: batchCost, batchOptimize: batchOptimize };
})();
if (typeof module !== 'undefined') module.exports = HE;

/* Ядро клиринга CE v2 по разбору «Клиринг CE: интерактивный разбор по функциональным блокам» (блоки 4–6):
   граф 6 узлов «актив@биржа» × 9 рёбер (6 переводчиков + 3 связки), система BGBᵀx = BGŝ с отчётным
   узлом SOL@OKX = 200 у.е., потоки w = α(ŝ − σ) и ведомость в штуках Δq = ±w/φ.
   Глубины α — базового сценария разбора (θ = 0,6, ψ_age 0,84/0,62, W_T/(γς²τ) = 50, связки 100). */

module.exports = HE;
