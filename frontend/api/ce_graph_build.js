// Граф CE: узлы — активы на биржах (asset@venue), рёбра — заявки агентов (CALC-CE-GRAPH).
// Только отображение/диагностика (Number допустим, CLAUDE.md §1.1) — в ledger не попадает.
//
// Переводчик T: ребро quote@v → base@v с якорем mid пары (в ед. quote), сдвинутым inventory-skew
//   anchor_eff = mid·exp(−clamp(γ·c)/1000), c = position+in_flight (k-USDT), как в matching.
//   Наклон: α_T (quote за б.п.) и β_T = mid²/(1e4·α_T) (цена за ед. base). Сопротивление
//   r = 100/α_USDT, б.п. на 100 USDT потока (α_USDT = α_T·цена quote в USDT).
// Арбитражёр A: ребро asset@src → asset@dst, якорь 0 (σ*=0, ce_graph_assembler), плечо из капитала:
//   α_A [тыс.USDT/‰] ⇒ α = 100·α_A USDT/б.п., r = 1/α_A б.п. на 100 USDT. Рассогласование цен
//   (ЭДС) = 1e4·ln(P_dst/P_src) б.п. — то, что гонит поток по ребру.

// deltas — дельты позиций агентов за последний такт клиринга (ledger GetAgentPositionDeltas,
// k-USDT, знак как у позиции c). Поток f по ветви положителен в направлении from → to:
//   переводчик (ветвь quote→base): c>0 означает «база продана за quote» ⇒ ценность идёт base→quote,
//     поэтому f = −Δc; арбитражёр: Δq_src=−f, Δq_dst=+f (ADR-069) ⇒ f = (Δdst − Δsrc)/2.
//
// Позиции у концов ветви (ends) — позиция агента В ВАЛЮТЕ каждого конца, в единицах валюты, со знаком
// (минус = короткая). Переводчик (c = position+in_flight, k-USDT; v = c·1000 USDT): база −v/P_base,
// quote +v/P_quote — те же ноги, что в панели «Позиции» (buildAgentRows.legs). Арбитражёр: реальные
// ноги ledger по биржам (legSrc/legDst, USDT) ÷ цена актива на своей бирже.
//
// Позиция агентов на внутренней CE-бирже по валюте узла (node.cePos): сумма позиций ВСЕХ агентов в валюте узла,
// т.е. сумма ends[] всех ветвей при узле (переводчик: база или quote по своему концу; арбитражёр: нога своей
// биржи); qty — итог, translator/arbitrageur — вклад видов агентов. Единицы — единицы валюты узла, знак как у ends.
//
// Позиция на внешней бирже от хеджирования (node.hedgePos): подтверждённые хедж-заполнения band-хеджей
// (venue_fill_diagnostics, ce|band|<агент>|…) с момента сброса статистики. Пара X/Y: X += BUY−SELL
// (кол-во базы), Y += выручка SELL − затраты BUY (в единицах quote). Разбивка по виду агента (T_/A_).
// hedgeFills — строки {agent_id, venue, symbol, buy_base, sell_base, buy_notional, sell_notional}.
function buildCeGraph({ segs = [], positions = [], deltas = [], hedgeFills = [], cfg = {} }) {
  const gamma = Number(cfg.gamma) || 0, clampPm = Number(cfg.clampPm) || 0;
  const transfer = cfg.transfer || {};
  const good = (s) => s && s.venue && /^[A-Z0-9]+\/[A-Z0-9]+$/.test(String(s.pair || '')) && Number(s.mid) > 0;
  const S = segs.filter(good);

  const byKey = new Map(S.map((s) => [`${s.venue}|${s.pair}`, s]));
  // цена актива на бирже в USDT: USDT=1; прямая пара X/USDT; иначе одна кросс-пара X/Q × цена Q.
  const priceOf = (asset, venue, depth = 0) => {
    if (asset === 'USDT') return 1;
    const direct = byKey.get(`${venue}|${asset}/USDT`);
    if (direct) return Number(direct.mid);
    if (depth > 0) return null;
    for (const s of S) {
      if (s.venue !== venue || !s.pair.startsWith(asset + '/')) continue;
      const q = s.pair.split('/')[1];
      const pq = priceOf(q, venue, 1);
      if (pq) return Number(s.mid) * pq;
    }
    return null;
  };

  const posByAgent = new Map();
  for (const p of positions) {
    if (!posByAgent.has(p.agent_id)) posByAgent.set(p.agent_id, []);
    posByAgent.get(p.agent_id).push(p);
  }
  const dByAgent = new Map();
  for (const d of deltas) {
    if (!dByAgent.has(d.agent_id)) dByAgent.set(d.agent_id, []);
    dByAgent.get(d.agent_id).push(d);
  }
  const dOf = (rows) => (rows || []).reduce((s, d) => s + (Number(d.delta) || 0), 0);
  const cOf = (rows) => (rows || []).reduce((s, p) => s + (Number(p.position) || 0) + (Number(p.in_flight) || 0), 0);

  const venues = [...new Set(S.map((s) => s.venue))].sort();
  const edges = [];
  const nodeSet = new Set();

  for (const s of S) {
    const [base, quote] = s.pair.split('/');
    const mid = Number(s.mid);
    const agentId = quote === 'USDT' ? `T_${base}_${s.venue}` : `T_${base}_${quote}_${s.venue}`;
    const rows = posByAgent.get(agentId);
    const c = cOf(rows);                                    // k-USDT
    const skewPm = Math.max(-clampPm, Math.min(clampPm, gamma * c));
    const anchorEff = mid * Math.exp(-skewPm / 1000);
    const alphaT = Number(s.alphaT), betaT = Number(s.betaT);
    const pq = priceOf(quote, s.venue);
    const alphaUsdt = alphaT > 0 && pq ? alphaT * pq : null;
    const from = `${quote}@${s.venue}`, to = `${base}@${s.venue}`;
    nodeSet.add(from); nodeSet.add(to);
    // позиция агента в валютах концов: цены из ledger (base_price, reference_price = P_base/P_quote),
    // иначе из сегментов клиринга; нет строки агента ⇒ позиции нет (null), а не фиктивный ноль
    const prow = (rows || []).find((r) => Number(r.base_price) > 0) || null;
    const pBase = prow ? Number(prow.base_price) : priceOf(base, s.venue);
    const pQuoteL = prow && Number(prow.reference_price) > 0 ? Number(prow.base_price) / Number(prow.reference_price) : null;
    const pQuote = pQuoteL || priceOf(quote, s.venue);
    const v = c * 1000;
    const ends = rows ? {
      from: { asset: quote, qty: pQuote > 0 ? v / pQuote : null },
      to: { asset: base, qty: pBase > 0 ? -v / pBase : null },
    } : null;
    edges.push({
      id: `T:${s.venue}:${s.pair}`, kind: 'translator', agentId, venue: s.venue, pair: s.pair, ends,
      base, quote, from, to, asset: base,
      anchor: mid, anchorEff, skewPm, alphaT: alphaT > 0 ? alphaT : null, betaT: betaT > 0 ? betaT : null,
      alphaUsdt, rBp100: alphaUsdt ? 100 / alphaUsdt : null, rBp1M: alphaUsdt ? 1e6 / alphaUsdt : null,
      positionUsdt: c * 1000, hasAgent: !!rows, eventMs: Number(s.eventMs) || 0,
      source: s.source === 'live' ? 'live' : 'clearing', levelsBid: s.levelsBid, levelsAsk: s.levelsAsk,
      flowUsdt: -dOf(dByAgent.get(agentId)) * 1000,
    });
  }

  // арбитражёры: A_<asset>_<src><dst> (имя двух бирж слитно), ноги — строки ledger по биржам.
  const arbAgents = new Map();
  for (const p of positions) {
    if (p.agent_kind !== 'arbitrageur') continue;
    if (!arbAgents.has(p.agent_id)) arbAgents.set(p.agent_id, []);
    arbAgents.get(p.agent_id).push(p);
  }
  for (const [agentId, rows] of arbAgents) {
    const asset = rows[0].asset;
    const rest = agentId.startsWith(`A_${asset}_`) ? agentId.slice(`A_${asset}_`.length) : '';
    let src = null, dst = null;
    for (const a of venues) for (const b of venues) if (a !== b && a + b === rest) { src = a; dst = b; }
    if (!src) continue;
    const tr = transfer[asset];
    const alphaA = tr && Number(tr.alpha) > 0 ? Number(tr.alpha) : null;
    const p1 = priceOf(asset, src), p2 = priceOf(asset, dst);
    const emfBp = p1 && p2 ? 1e4 * Math.log(p2 / p1) : null;
    const leg = (v) => cOf(rows.filter((r) => r.venue === v)) * 1000;
    const from = `${asset}@${src}`, to = `${asset}@${dst}`;
    nodeSet.add(from); nodeSet.add(to);
    const unit = (usdt, price) => (asset === 'USDT' ? usdt : (price > 0 ? usdt / price : null));
    const ends = { from: { asset, qty: unit(leg(src), p1) }, to: { asset, qty: unit(leg(dst), p2) } };
    edges.push({
      id: `A:${agentId}`, kind: 'arbitrageur', agentId, asset, src, dst, from, to, ends,
      anchor: 0, alphaA, alphaUsdt: alphaA ? alphaA * 100 : null, rBp100: alphaA ? 1 / alphaA : null, rBp1M: alphaA ? 1e4 / alphaA : null,
      deadZonePm: tr && Number(tr.c) >= 0 ? Number(tr.c) : null, emfBp,
      legSrcUsdt: leg(src), legDstUsdt: leg(dst),
      flowUsdt: (dOf((dByAgent.get(agentId) || []).filter((d) => d.venue === dst))
                 - dOf((dByAgent.get(agentId) || []).filter((d) => d.venue === src))) / 2 * 1000,
    });
  }

  // позиции на внешних биржах от хеджирования: asset@venue → {qty, translator, arbitrageur}
  const splitSymbol = (symbol) => {
    const key = String(symbol || '').replace('/', '');
    const known = S.find((s) => s.pair.replace('/', '') === key);
    if (known) { const [b, q] = known.pair.split('/'); return { base: b, quote: q }; }
    for (const q of ['USDT', 'BTC', 'ETH']) {
      if (key.length > q.length && key.endsWith(q)) return { base: key.slice(0, key.length - q.length), quote: q };
    }
    return null;
  };
  const hedge = new Map();
  const addHedge = (asset, venue, kind, qty) => {
    const id = `${asset}@${venue}`;
    if (!hedge.has(id)) hedge.set(id, { qty: 0, translator: 0, arbitrageur: 0 });
    const h = hedge.get(id); h.qty += qty; h[kind] += qty;
  };
  for (const f of hedgeFills) {
    const sp = splitSymbol(f.symbol);
    if (!sp || !f.venue) continue;
    const kind = String(f.agent_id || '').startsWith('A_') ? 'arbitrageur' : 'translator';
    addHedge(sp.base, f.venue, kind, (Number(f.buy_base) || 0) - (Number(f.sell_base) || 0));
    addHedge(sp.quote, f.venue, kind, (Number(f.sell_notional) || 0) - (Number(f.buy_notional) || 0));
  }

  const nodes = [...nodeSet].map((id) => {
    const [asset, venue] = id.split('@');
    // чистый приток ценности в узел за такт (USDT): входящие потоки минус исходящие
    const net = edges.reduce((s, e) => s + (e.to === id ? e.flowUsdt : 0) - (e.from === id ? e.flowUsdt : 0), 0);
    const hp = hedge.get(id) || { qty: 0, translator: 0, arbitrageur: 0 };
    // позиции агентов на внутренней CE-бирже: каждый конец ветви при узле вносит позицию своего агента в валюте узла
    const ce = { qty: 0, translator: 0, arbitrageur: 0 };
    for (const e of edges) {
      for (const [w, nodeId] of [['from', e.from], ['to', e.to]]) {
        const q = nodeId === id && e.ends && e.ends[w];
        if (!q || !Number.isFinite(q.qty)) continue;
        ce.qty += q.qty; ce[e.kind] += q.qty;
      }
    }
    return { id, asset, venue, priceUsdt: priceOf(asset, venue), netFlowUsdt: net, hedgePos: hp, cePos: ce };
  });
  const assets = [...new Set(nodes.map((n) => n.asset))];
  const pairs = [...new Set(edges.filter((e) => e.kind === 'translator').map((e) => e.pair))].sort();
  return { nodes, edges, venues, assets, pairs };
}

module.exports = { buildCeGraph };
