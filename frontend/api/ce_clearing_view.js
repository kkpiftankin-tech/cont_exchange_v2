'use strict';
// ADR-079 — вид «решение клиринга + ликвидность бирж» для вкладки «Граф».
//
// Источник решения — снимок matching (GET /ce/clearing/last): потенциалы узлов, по каждому ребру отклонение d,
// полка c (и её состав), зазор, поток, состояние; типовые циклы (G, порог Σc, запас); отброшенные котировки.
// Здесь ничего не пересчитывается и не выдумывается: числа переносятся как есть, добавляются только пояснения
// словами («почему потока нет») и сведение с состоянием бирж из админ-API venues. Нет данных — поле null и
// честное пояснение, а не нули.

const num = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null);
const bp = (pm) => (num(pm) == null ? null : pm * 10);          // ‰ → базисные пункты (1‰ = 10 б.п.)
const f3 = (v) => (num(v) == null ? '—' : (Math.abs(v) >= 100 ? v.toFixed(0) : Math.abs(v) >= 1 ? v.toFixed(2) : v.toFixed(3)));
const pmTxt = (pm) => (num(pm) == null ? '—' : `${f3(pm)}‰ (${f3(pm * 10)} б.п.)`);

function enrichEdges(edges) {
  return (Array.isArray(edges) ? edges : []).map((e) => ({
    ...e,
    margin_bp: bp(e.margin_pm),
    deviation_bp: bp(e.deviation_pm),
    dead_zone_bp: bp(e.dead_zone_pm),
    // полка арбитражёра — это c_A (издержки перевода, CE_TRANSFER_C), у переводчика — состав из age/спред/комиссия
    dz_transfer_pm: e.kind === 'arbitrageur' ? e.dead_zone_pm : 0,
  }));
}

function enrichCycles(cycles, edgesByName) {
  return (Array.isArray(cycles) ? cycles : []).map((c) => {
    const parts = (c.parts || []).map((p) => {
      const e = edgesByName.get(p.edge) || {};
      return {
        ...p,
        kind: e.kind || null,
        dz_age_pm: num(e.dz_age_pm) || 0,
        dz_spread_pm: num(e.dz_spread_pm) || 0,
        dz_commission_pm: num(e.dz_commission_pm) || 0,
        dz_transfer_pm: e.kind === 'arbitrageur' ? (num(p.dead_zone_pm) || 0) : 0,
      };
    });
    const sum = (k) => parts.reduce((s, p) => s + (num(p[k]) || 0), 0);
    return {
      ...c,
      drive_bp: bp(c.drive_pm), threshold_bp: bp(c.threshold_pm), margin_bp: bp(c.margin_pm),
      parts,
      thr_age_pm: sum('dz_age_pm'), thr_spread_pm: sum('dz_spread_pm'),
      thr_commission_pm: sum('dz_commission_pm'), thr_transfer_pm: sum('dz_transfer_pm'),
    };
  });
}

// Решение старше этого возраста считается устаревшим: matching не публикует снимок, пока клиринг не получил ни
// одной котировки (все кривые устарели или отброшены), и тогда на странице остаётся прошлый такт.
const STALE_SOLVE_MS = 30000;

// Пояснение словами: что решил клиринг и почему потока нет/есть. ageMs — возраст решения (null, если неизвестен).
function explain(solve, cycles, dropped, venueIssues, ageMs = null) {
  const bullets = [];
  if (!solve) {
    return { level: 'error', headline: 'Клиринг не прислал решение: нет снимка от matching (сервис не поднят, CE-клиринг выключен или ещё не было такта).', bullets };
  }
  const s = solve.summary || {};
  if (!solve.converged) {
    bullets.push({ level: 'error', text: `Решатель не сошёлся за ${solve.iters} итераций (невязка баланса ${f3(solve.max_imbalance)}) — потоки этого такта недостоверны.` });
  }
  const active = num(s.active) || 0;
  const total = num(s.edges) || 0;
  const activeArb = num(s.active_arbitrageurs) || 0;
  if (total === 0) {
    return { level: 'error', headline: 'Снимок решения пуст: в графе клиринга нет ни одного ребра (котировки не пришли или все отброшены).', bullets };
  }
  let level = 'ok';
  let headline;
  if (active > 0) {
    const flow = (solve.edges || []).reduce((a, e) => a + Math.abs(num(e.flow_kusdt) || 0), 0) * 1000;
    headline = `Поток есть: активны ${active} из ${total} рёбер (переводчиков ${s.active_translators}, арбитражёров ${s.active_arbitrageurs}), суммарный |поток| такта ${flow >= 0.01 ? `≈ ${f3(flow)}` : 'меньше 0,01'} USDT.`;
    level = 'ok';
    if ((num(s.cycles_open) || 0) === 0 && activeArb === 0) {
      bullets.push({ level: 'info', text: 'Арбитражных рёбер между биржами в этом такте нет — поток идёт внутри бирж (между переводчиками одной площадки).' });
    }
  } else {
    level = 'warn';
    const inDead = num(s.in_dead_zone) || 0;
    const noDepth = num(s.no_depth) || 0;
    const invalid = num(s.invalid) || 0;
    const zero = (solve.nodes || []).every((n) => Math.abs(num(n.potential_pm) || 0) < 1e-9);
    const parts = [`${inDead} из ${total} рёбер внутри своих полок (отклонение |d| не превышает полку c)`];
    if (noDepth > 0) parts.push(`${noDepth} без глубины (ребро не способно нести поток)`);
    if (invalid > 0) parts.push(`${invalid} с некорректным отклонением (NaN во входе)`);
    headline = `Потока нет: ${parts.join(', ')}; ${zero ? 'потенциалы узлов равны нулю — решение «ничего не делать» корректно для таких входов' : 'потенциалы узлов не нулевые, но ни одно ребро не превысило свою полку'}.`;
    if (s.nearest_edge) {
      bullets.push({ level: 'info', text: `Ближе всех к активации ребро ${s.nearest_edge}: до его полки не хватает ${pmTxt(s.nearest_margin_pm)}.` });
    }
  }
  const open = num(s.cycles_open) || 0, ct = num(s.cycles_total) || 0;
  if (ct > 0) {
    const blocked = cycles.filter((c) => c.state === 'blocked').sort((a, b) => a.margin_pm - b.margin_pm);
    const noDepthCycles = cycles.filter((c) => c.state === 'no_depth').length;
    const worst = blocked[0];
    if (open === 0 && activeArb === 0 && worst) {
      bullets.push({
        level: 'warn',
        text: `Межбиржевой арбитраж заблокирован: из ${ct} типовых циклов (актив × пара бирж через USDT) открытых 0. Ближайший — ${worst.asset} ${worst.venue_a}↔${worst.venue_b}: разрыв цен ${pmTxt(Math.abs(worst.drive_pm))} против порога ${pmTxt(worst.threshold_pm)} — не хватает ${pmTxt(worst.margin_pm)}.`,
      });
      const thr = worst.threshold_pm || 0;
      const part = (v, name) => (thr > 0 && v > 0 ? `${name} ${pmTxt(v)} (${Math.round((100 * v) / thr)} %)` : null);
      const pieces = [
        part(worst.thr_age_pm, 'полка по возрасту данных ADR-075 у двух переводчиков'),
        part(worst.thr_spread_pm, 'полка ½спреда у переводчиков'),
        part(worst.thr_commission_pm, 'комиссия в полке'),
        part(worst.thr_transfer_pm, 'издержки перевода между биржами (CE_TRANSFER_C актива и USDT)'),
      ].filter(Boolean);
      if (pieces.length) bullets.push({ level: 'info', text: `Из чего сложен порог этого цикла: ${pieces.join('; ')}.` });
    } else if (open === 0 && activeArb > 0) {
      bullets.push({ level: 'info', text: `Открытых типовых циклов 0 из ${ct} (актив × пара бирж через USDT), но арбитражёры активны — их поток идёт по путям, которых типовые циклы не охватывают.` });
    } else if (open > 0) {
      bullets.push({ level: 'ok', text: `Открытых типовых циклов: ${open} из ${ct}.` });
    }
    if (noDepthCycles > 0) {
      bullets.push({ level: 'warn', text: `${noDepthCycles} из ${ct} типовых циклов не могут открыться: у одного из четырёх рёбер нет глубины (ликвидности).` });
    }
  }
  if ((dropped || []).length > 0) {
    const by = {};
    for (const d of dropped) by[d.reason] = (by[d.reason] || 0) + 1;
    const names = { stale: 'устарели', thin_depth: 'слишком тонкие по глубине', invalid_depth: 'нулевая глубина' };
    bullets.push({ level: 'warn', text: `В клиринг не попали котировки: ${Object.entries(by).map(([r, n]) => `${n} — ${names[r] || r}`).join(', ')} (список — в таблице ликвидности).` });
  }
  for (const t of venueIssues) bullets.push({ level: 'warn', text: t });
  if (num(ageMs) != null && ageMs > STALE_SOLVE_MS) {
    // Снимок не обновляется: всё, что выше, — про ПРОШЛЫЙ такт. Главный вывод заменяется, прежний уходит в пункт списка.
    bullets.unshift({ level: 'info', text: `Последнее известное решение (${f3(ageMs / 1000)} с назад): ${headline}` });
    level = 'error';
    headline = `Решение клиринга устарело: последний такт посчитан ${f3(ageMs / 1000)} с назад, новые такты не приходят (вероятно, все котировки устарели или отброшены, либо цепочка остановлена). Ниже — прошлое решение, не текущее.`;
  }
  return { level, headline, bullets };
}

// solve — JSON matching или null; adminItems — items из /api/v1/venues venues; nowMs — «сейчас» BFF.
function buildClearingView({ solve = null, adminItems = [], nowMs = Date.now(), solveError = null } = {}) {
  const edges = enrichEdges(solve && solve.edges);
  const edgesByName = new Map(edges.map((e) => [e.name, e]));
  const cycles = enrichCycles(solve && solve.cycles, edgesByName);
  const dropped = Array.isArray(solve && solve.dropped) ? solve.dropped : [];
  const adminById = new Map();
  for (const it of Array.isArray(adminItems) ? adminItems : []) {
    const id = it && it.config && it.config.venue_id;
    if (id) adminById.set(id, it);
  }

  // ликвидность: по каждой бирже — состояние и пары (клиринг ↔ опрос)
  const venueIds = new Set([...adminById.keys(), ...edges.filter((e) => e.kind === 'translator').map((e) => e.venue), ...dropped.map((d) => d.venue)]);
  const venueIssues = [];
  const venues = [...venueIds].sort().map((vid) => {
    const it = adminById.get(vid) || null;
    const health = (it && it.health) || {};
    const poll = (it && it.metrics && it.metrics.poll) || null;
    const active = it && it.config ? it.config.is_active !== false : null;
    const breaker = health.circuit_breaker_state || null;
    if (active === false) venueIssues.push(`Биржа ${vid} отключена оператором — её котировки в клиринге не участвуют.`);
    else if (breaker && breaker !== 'CLOSED') venueIssues.push(`У биржи ${vid} предохранитель ${breaker} (${health.circuit_breaker_reason || 'причина не указана'}) — снимки не обновляются.`);
    const symbols = new Set();
    for (const e of edges) if (e.kind === 'translator' && e.venue === vid) symbols.add(`${e.asset}/${e.quote}`);
    for (const d of dropped) if (d.venue === vid) symbols.add(`${d.asset}/${d.quote}`);
    if (poll && poll.pairs) for (const k of Object.keys(poll.pairs)) symbols.add(k);
    const pairs = [...symbols].sort().map((sym) => {
      const [base, quote] = sym.split('/');
      const edge = edges.find((e) => e.kind === 'translator' && e.venue === vid && e.asset === base && e.quote === quote) || null;
      const drop = dropped.find((d) => d.venue === vid && d.asset === base && d.quote === quote) || null;
      const pp = poll && poll.pairs && poll.pairs[sym] ? poll.pairs[sym] : null;
      let status = 'нет данных';
      if (edge) status = edge.state === 'active' ? 'в клиринге, поток есть' : edge.state === 'dead_zone' ? 'в клиринге, внутри полки' : 'в клиринге, нет глубины';
      else if (drop) status = drop.reason === 'stale' ? 'отброшена: устарела' : drop.reason === 'thin_depth' ? 'отброшена: тонкая' : 'отброшена: нулевая глубина';
      return {
        symbol: sym,
        status,
        in_clearing: !!edge,
        edge: edge ? edge.name : null,
        state: edge ? edge.state : null,
        depth: edge ? num(edge.depth) : (drop ? num(drop.depth) : null),
        dead_zone_pm: edge ? num(edge.dead_zone_pm) : null,
        dz_age_pm: edge ? num(edge.dz_age_pm) : null,
        dz_spread_pm: edge ? num(edge.dz_spread_pm) : null,
        dz_commission_pm: edge ? num(edge.dz_commission_pm) : null,
        sigma_pm: edge ? num(edge.sigma_pm) : null,
        clearing_age_ms: edge ? num(edge.age_ms) : (drop ? num(drop.age_ms) : null),
        dropped_reason: drop ? drop.reason : null,
        dropped_threshold: drop ? num(drop.threshold) : null,
        book_age_s: pp ? num(pp.age_s) : null,
        poll_period_s: pp ? num(pp.period_s) : null,
        poll_health: pp && pp.health != null ? pp.health : null,   // 0 свежая, 1 стареющая, 2 устаревшая, 3 недоступна
        poll_fails: pp ? num(pp.fails) : null,
      };
    });
    return {
      venue_id: vid,
      active,
      status: health.status || null,
      breaker,
      breaker_reason: health.circuit_breaker_reason || null,
      stale_ms: num(health.stale_ms),
      poll_mode: poll ? poll.mode || null : null,
      poll_period_s: poll ? num(poll.period_s) : null,
      poll_paused: poll ? !!poll.paused : null,
      pairs,
    };
  });

  const nodes = Array.isArray(solve && solve.nodes) ? solve.nodes : [];
  const ageMs = solve && num(solve.solved_at_ms) ? Math.max(0, nowMs - solve.solved_at_ms) : null;
  return {
    available: !!solve,
    error: solve ? null : (solveError || 'no_snapshot'),
    solve: solve ? {
      batchId: solve.batch_id, eventTimeMs: num(solve.event_time_ms), solvedAtMs: num(solve.solved_at_ms),
      ageMs,
      numeraire: solve.numeraire, converged: !!solve.converged, iters: num(solve.iters), maxImbalance: num(solve.max_imbalance),
      quotesReceived: num(solve.quotes_received), summary: solve.summary || {},
      nodes, edges, cycles,
    } : null,
    explanation: explain(solve, cycles, dropped, venueIssues, ageMs),
    liquidity: { venues, dropped },
    generatedAt: new Date(nowMs).toISOString(),
  };
}

module.exports = { buildClearingView, explain, enrichCycles, enrichEdges, STALE_SOLVE_MS };
