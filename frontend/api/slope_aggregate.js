// Суммарные линейные кривые ликвидности бирж (CALC-F05A-SLOPE-ONDEMAND §агрегат).
// Только отображение/диагностика — в ledger не попадает (Number допустим, CLAUDE.md §1.1).
//
// Кривая биржи i на выбранном способе §6.2 — прямая P(q) = anchor_i + β_i·q, q со знаком
// (ask +, bid −). В осях «цена → объём» это q_i(p) = (p − anchor_i)/β_i = k_i·p + c_i,
// где k_i = 1/β_i, c_i = −anchor_i/β_i. Объёмы бирж при одной цене складываются, поэтому
// сумма прямых — снова прямая: q_Σ(p) = (Σk_i)·p + Σc_i. Та же формула, что в
// fetchAggregateCurves (вкладка Clearing, ADR-053): Σ1/β·p − Σ anchor/β.
//
// venues — [{ venue, anchor, betaT, ... }]; биржи с β≤0 или нефинитным anchor пропускаются.
// exclude — id выбранной биржи: «все кроме выбранной» = Σ без неё (если её нет — равно «все»).
function aggregateLines(venues, exclude) {
  const lines = [];
  for (const v of Array.isArray(venues) ? venues : []) {
    const beta = Number(v && v.betaT), anchor = Number(v && v.anchor);
    if (!(beta > 0) || !Number.isFinite(beta) || !Number.isFinite(anchor)) continue;
    lines.push({ venue: v.venue, anchor, betaT: beta, k: 1 / beta, c: -anchor / beta });
  }
  const sum = (arr) => ({
    k: arr.reduce((s, l) => s + l.k, 0),
    c: arr.reduce((s, l) => s + l.c, 0),
    count: arr.length,
  });
  const all = sum(lines);
  const rest = lines.filter((l) => l.venue !== exclude);
  const ex = { ...sum(rest), excluded: lines.some((l) => l.venue === exclude) ? exclude : null };
  return { lines, all, ex };
}

module.exports = { aggregateLines };
