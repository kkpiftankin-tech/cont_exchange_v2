import React, { useEffect, useMemo, useState, useCallback, useRef } from 'react';
import NavBar from '../../components/NavBar';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import CeClearingPanel from './CeClearingPanel';
import './CeGraphLive.css';

// Граф CE: узлы — активы на биржах (asset@venue), рёбра — заявки агентов (переводчики T внутри
// биржи, арбитражёры A между биржами). Чистый рендер ответа BFF /vector-clearing/ce-graph:
// якоря, наклоны, ЭДС, сопротивления, позиции агентов в валютах концов ветвей и позиции на внешних
// биржах от хеджирования считает бэкенд (ce_graph_build.js, CALC-CE-GRAPH), здесь только раскладка
// и фильтры видимости.
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_MS = 3000;
const LEASE_HEARTBEAT_MS = 2500;   // как в панели «Торговля»: аренда глубокого опроса подтверждается каждые 2,5 с
const LEASE_TTL_MS = 6000;         // без сигналов быстрая полоса гаснет сама за ≤6 с
const PAIR_PRIORITY = ['BTC/USDT', 'ETH/USDT', 'SOL/USDT', 'ETH/BTC', 'SOL/BTC'];
const ASSET_ORDER = ['USDT', 'BTC', 'ETH', 'SOL'];
const VENUE_COLORS = { binance: '#f3ba2f', okx: '#7aa2ff', kraken: '#b08cff', coinbase: '#4aa3ff', uniswap_v3: '#ff6ec7' };
const ASSET_COLORS = { USDT: '#26a17b', BTC: '#f7931a', ETH: '#8a9bff', SOL: '#14c9a5' };
const vColor = (v) => VENUE_COLORS[v] || '#9aa7b4';
const aColor = (a) => ASSET_COLORS[a] || '#9aa7b4';

const fmt = (v, d = 4) => {
  const n = Number(v);
  if (!Number.isFinite(n)) return '—';
  const a = Math.abs(n);
  if (a !== 0 && (a < 1e-3 || a >= 1e7)) return n.toExponential(3);
  return n.toLocaleString('ru-RU', { maximumFractionDigits: a >= 1000 ? 2 : d });
};
const sgn = (v, d = 3) => (Number.isFinite(Number(v)) ? (v > 0 ? '+' : '') + fmt(v, d) : '—');

// Количество валюты со знаком, компактно (для подписей у узла и внутри кружка): +1,23M / −12,3k / +0,0123 / 0.
// Минус — короткая позиция. Только отображение: единицы и знак посчитаны бэкендом.
const fmtQty = (v) => {
  const n = Number(v);
  if (v == null || !Number.isFinite(n)) return '—';
  if (n === 0) return '0';
  const a = Math.abs(n), s = n > 0 ? '+' : '−';
  let t;
  if (a >= 1e6) t = (a / 1e6).toLocaleString('ru-RU', { maximumFractionDigits: 3 }) + 'M';
  else if (a >= 1e4) t = (a / 1e3).toLocaleString('ru-RU', { maximumFractionDigits: 2 }) + 'k';
  else if (a >= 100) t = a.toLocaleString('ru-RU', { maximumFractionDigits: 1 });
  else if (a >= 1) t = a.toLocaleString('ru-RU', { maximumFractionDigits: 3 });
  else t = Number(a.toPrecision(3)).toLocaleString('ru-RU', { maximumFractionDigits: 9 });
  return s + t;
};

// Точка на квадратичной кривой Безье и касательная (для подписей и точек потока вдоль ребра).
const qpt = (P0, P1, P2, t) => {
  const u = 1 - t;
  return [u * u * P0[0] + 2 * u * t * P1[0] + t * t * P2[0], u * u * P0[1] + 2 * u * t * P1[1] + t * t * P2[1]];
};
const qtan = (P0, P1, P2, t) => {
  const dx = 2 * (1 - t) * (P1[0] - P0[0]) + 2 * t * (P2[0] - P1[0]);
  const dy = 2 * (1 - t) * (P1[1] - P0[1]) + 2 * t * (P2[1] - P1[1]);
  const l = Math.hypot(dx, dy) || 1;
  return [dx / l, dy / l];
};
// Длина кривой (численно) → t по длине дуги.
const arcT = (P0, P1, P2, frac) => {
  const N = 40; const pts = [qpt(P0, P1, P2, 0)]; let tot = 0; const acc = [0];
  for (let i = 1; i <= N; i++) { const p = qpt(P0, P1, P2, i / N); tot += Math.hypot(p[0] - pts[i - 1][0], p[1] - pts[i - 1][1]); pts.push(p); acc.push(tot); }
  const target = frac * tot; let i = 1; while (i < N && acc[i] < target) i++;
  const seg = acc[i] - acc[i - 1] || 1; return { t: (i - 1 + (target - acc[i - 1]) / seg) / N, len: tot };
};

// Поток по ветви: положителен в направлении from → to. Скорость и число точек растут с |f| (логарифм).
// Масштаб относительно самого крупного потока видимых ветвей (потоки такта — доли и единицы USDT,
// абсолютная шкала их не различила бы): u = |f|/max ∈ (0,1].
const flowU = (f, maxF) => (maxF > 0 ? Math.min(1, Math.abs(f) / maxF) : 0);
const flowDur = (f, maxF) => 5.2 - 3.9 * Math.sqrt(flowU(f, maxF));
const flowDots = (f, maxF) => 1 + Math.floor(3 * flowU(f, maxF));
const flowW = (f, maxF) => 2 + 2.4 * Math.sqrt(flowU(f, maxF));
const fmtUsd = (v) => { const a = Math.abs(Number(v)); return Number.isFinite(a) ? (a >= 100 ? fmt(a, 0) : a >= 1 ? fmt(a, 1) : fmt(a, 3)) : '—'; };
const EPS = { anchor: 1e-9, r: 1e-6, price: 1e-9 };
const CHG_MS = 15000;   // сколько секунд показывать «шаг» последнего изменения позиции
const rel = (a, b) => (Number.isFinite(a) && Number.isFinite(b) && b !== 0 ? (a - b) / Math.abs(b) : 0);

// Плашка «что изменилось»: рамка цвета направления + ▲/▼ + ТОЛЬКО шаг (без кода валюты и знака), чтобы её нельзя было
// принять за ещё одну позицию рядом с числами в кружке. tag — к какой позиции относится шаг («CE» / «бирж.»).
const DeltaPill = ({ x, y, tag, d, cls }) => {
  const col = d > 0 ? '#5fd08a' : '#e6725a';
  const txt = `${tag ? tag + ' ' : ''}${d > 0 ? '▲' : '▼'}${fmtQty(d).slice(1)}`;
  const w = txt.length * 5.3 + 8;
  return (
    <g className="cg-delta">
      <rect x={x - w / 2} y={y - 9} width={w} height={12} rx={4} fill="#0b0f14" stroke={col} strokeWidth={1} />
      <text x={x} y={y} textAnchor="middle" className={cls} fill={col}>{txt}</text>
    </g>
  );
};

const CeGraphLive = () => {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [data, setData] = useState(null);
  const [err, setErr] = useState(null);
  const [hidden, setHidden] = useState({ venues: {}, assets: {}, pairs: {}, kinds: {} });
  const [sel, setSel] = useState(null);       // { type: 'edge'|'node', id }
  const [now, setNow] = useState(Date.now());
  const [labels, setLabels] = useState('flow');  // flow | none
  const [chg, setChg] = useState({ edges: {}, nodes: {} });  // что изменилось при последнем пересчёте
  const [beat, setBeat] = useState(0);
  // Быстрая полоса (ADR-070): якоря/наклоны пересчитываются по глубокому стакану ~каждые 2–3 с
  const [liveOn, setLiveOn] = useState(false);
  const [lease, setLease] = useState(null);
  const prev = useRef(null);
  // Последнее изменение каждой видимой позиции (подписи у узлов и число в кружке): {d: на сколько, ts: когда}.
  // Шаг позиции за такт мал (доли единицы валюты) — само число почти не меняется, поэтому изменение
  // подсвечивается и подписывается ▲/▼ с величиной шага (CHG_MS).
  const posPrev = useRef({});
  const posChg = useRef({});

  useEffect(() => { isAuthenticated().then((a) => { setIsAuth(a); if (!a) navigate('/login'); }); }, [navigate]);
  const load = useCallback(async () => {
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/ce-graph`, { params: { live: liveOn ? 1 : undefined }, timeout: 15000 });
      const d = r.data;
      // «Дыхание»: сравниваем с предыдущим пересчётом — ветви и узлы с изменившимися якорем,
      // наклоном или ценой получают пульс и стрелку изменения (▲/▼). Только отображение разницы.
      const p = prev.current;
      const e = {}, n = {};
      if (p) {
        for (const x of d.edges) {
          const o = p.edges[x.id]; if (!o) continue;
          const a1 = x.kind === 'translator' ? x.anchorEff : x.emfBp, a0 = o.kind === 'translator' ? o.anchorEff : o.emfBp;
          const dA = x.kind === 'translator' ? rel(a1, a0) : (Number.isFinite(a1) && Number.isFinite(a0) ? a1 - a0 : 0);
          const dR = rel(x.rBp1M, o.rBp1M), dF = Number.isFinite(x.flowUsdt) && Number.isFinite(o.flowUsdt) ? x.flowUsdt - o.flowUsdt : 0;
          const thrA = x.kind === 'translator' ? EPS.anchor : 1e-6;
          if (Math.abs(dA) > thrA || Math.abs(dR) > EPS.r || Math.abs(dF) > 1e-6) e[x.id] = { dA, dR, dF };
        }
        for (const x of d.nodes) {
          const o = p.nodes[x.id];
          if (o && Math.abs(rel(x.priceUsdt, o.priceUsdt)) > EPS.price) n[x.id] = { dP: rel(x.priceUsdt, o.priceUsdt) };
        }
      }
      prev.current = { edges: Object.fromEntries(d.edges.map((x) => [x.id, x])), nodes: Object.fromEntries(d.nodes.map((x) => [x.id, x])) };
      const stamp = Date.now();
      const track = (key, val, unit) => {
        if (val == null || !Number.isFinite(Number(val))) return;
        const o = posPrev.current[key];
        if (o != null && Math.abs(val - o) > Math.max(1e-12, 1e-9 * Math.abs(o))) posChg.current[key] = { d: val - o, ts: stamp, unit };
        posPrev.current[key] = val;
      };
      for (const x of d.edges) for (const w of ['from', 'to']) { const q = x.ends && x.ends[w]; if (q) track(`e|${x.id}|${w}`, q.qty, q.asset); }
      for (const x of d.nodes) {
        if (x.hedgePos) track(`n|${x.id}`, x.hedgePos.qty, x.asset);
        if (x.cePos) track(`c|${x.id}`, x.cePos.qty, x.asset);
      }
      setChg((c) => ({ edges: e, nodes: n, tick: (c.tick || 0) + 1 }));
      setBeat((b) => b + 1);
      setData(d); setErr(null);
    }
    catch (e) { setErr('не удалось загрузить граф'); }
  }, [liveOn]);
  useEffect(() => { prev.current = null; }, [liveOn]);   // смена источника — не считать разницу «вспышками»
  useEffect(() => { if (isAuth) load(); }, [isAuth, load]);
  useInterval(load, isAuth ? POLL_MS : null);
  useInterval(() => setNow(Date.now()), 1000);
  const toggle = (grp, k) => setHidden((h) => ({ ...h, [grp]: { ...h[grp], [k]: !h[grp][k] } }));

  // Видимость: скрытая биржа/актив убирает узлы и все рёбра при них; скрытая пара — рёбра пары.
  const view = useMemo(() => {
    if (!data) return null;
    const assets = [...data.assets].sort((a, b) => {
      const ia = ASSET_ORDER.indexOf(a), ib = ASSET_ORDER.indexOf(b);
      return (ia < 0 ? 99 : ia) - (ib < 0 ? 99 : ib) || a.localeCompare(b);
    });
    const vVis = data.venues.filter((v) => !hidden.venues[v]);
    const aVis = assets.filter((a) => !hidden.assets[a]);
    const nodeVisible = (n) => !hidden.venues[n.venue] && !hidden.assets[n.asset];
    const nodes = data.nodes.filter(nodeVisible);
    const ids = new Set(nodes.map((n) => n.id));
    const edges = data.edges.filter((e) => ids.has(e.from) && ids.has(e.to) && !hidden.kinds[e.kind]
      && !(e.kind === 'translator' && hidden.pairs[e.pair]));
    return { assets, vVis, aVis, nodes, edges };
  }, [data, hidden]);

  // Аренда: только видимые переводчики (venue, pair), по приоритету пар; сверх потолка venues отклоняет.
  const leaseItems = useMemo(() => {
    if (!view) return [];
    const seen = new Set(), out = [];
    for (const e of view.edges) {
      if (e.kind !== 'translator') continue;
      const k = `${e.venue}|${e.pair}`;
      if (!seen.has(k)) { seen.add(k); out.push({ venue: e.venue, symbol: e.pair }); }
    }
    const pr = (x) => { const i = PAIR_PRIORITY.indexOf(x.symbol); return i < 0 ? 99 : i; };
    return out.sort((a, b) => pr(a) - pr(b) || a.venue.localeCompare(b.venue));
  }, [view]);
  const leaseRef = useRef([]);
  leaseRef.current = leaseItems;
  const sendLease = useCallback(async () => {
    const items = leaseRef.current;
    if (!items.length) return;
    try {
      const r = await axios.post(`${API_BASE}/venues/depth-watch`, { items, ttl_ms: LEASE_TTL_MS }, { timeout: 8000 });
      setLease({ ok: true, requested: items.length, ...r.data });
    } catch (e) {
      const code = e.response && e.response.data && e.response.data.error;
      setLease({ ok: false, error: code === 'endpoint_unavailable' ? 'сервис venues не поддерживает быструю полосу' : 'не удалось включить быструю полосу' });
    }
  }, []);
  useEffect(() => { if (liveOn) sendLease(); else setLease(null); }, [liveOn, sendLease]);
  useInterval(sendLease, liveOn ? LEASE_HEARTBEAT_MS : null);

  if (isAuth === null) return <div className="cg-page"><NavBar /></div>;

  const W = 1000, colW = view && view.vVis.length ? Math.min(240, (W - 140) / view.vVis.length) : 200;
  const rowH = 128, top = 70, left = 120;
  const colX = (v) => left + view.vVis.indexOf(v) * colW + colW / 2;
  const rowY = (a) => top + view.aVis.indexOf(a) * rowH + 30;
  const H = view ? top + Math.max(1, view.aVis.length) * rowH : 300;
  const pos = (id) => { const [a, v] = id.split('@'); return [colX(v), rowY(a)]; };

  const edgeGeom = (e) => {
    const A = pos(e.from), B = pos(e.to);
    let C;
    if (e.kind === 'translator') {
      const dist = Math.abs(view.aVis.indexOf(e.base) - view.aVis.indexOf(e.quote));
      const side = e.quote === view.aVis[0] ? -1 : 1;
      const off = dist <= 1 ? 0 : side * (34 + 24 * (dist - 1));
      C = [(A[0] + B[0]) / 2 + off, (A[1] + B[1]) / 2];
    } else {
      const dist = Math.abs(view.vVis.indexOf(e.src) - view.vVis.indexOf(e.dst));
      const off = dist <= 1 ? 0 : (dist % 2 === 0 ? -1 : 1) * (36 + 20 * (dist - 2));
      C = [(A[0] + B[0]) / 2, (A[1] + B[1]) / 2 + off];
    }
    return { A, B, C };
  };
  const edgeColor = (e) => (e.kind === 'translator' ? vColor(e.venue) : aColor(e.asset));
  // Позиция агента в валюте конца ветви — рядом с узлом, чуть в стороне от линии. Ветвей у узла бывает 5–7, поэтому
  // подписи раскладываются жадно (только геометрия отображения): для каждой перебираются расстояния от центра узла
  // (шаг 12 px) и сторона от линии, берётся первое место, не пересекающее уже поставленные подписи и узлы
  // (кружок, цена, поток). Порядок — по id ветви, ширина — по числу знаков, округлённому до 4: подписи не «прыгают»
  // при каждом обновлении чисел.
  const NODE_R = 24;
  const LBL_FS = 8.5, LBL_CH = LBL_FS * 0.62;
  const endText = (e, which) => {
    const q = e.ends && e.ends[which];
    return q && q.qty != null ? `${fmtQty(q.qty)} ${q.asset}` : null;
  };
  const labelPos = {};
  if (view) {
    const hit = (u, v) => u.x0 < v.x1 && v.x0 < u.x1 && u.y0 < v.y1 && v.y0 < u.y1;
    const obst = view.nodes.map((n) => { const x = colX(n.venue), y = rowY(n.asset); return { x0: x - NODE_R, x1: x + NODE_R, y0: y - NODE_R, y1: y + NODE_R + 30 }; });
    const placed = [];
    for (const e of [...view.edges].sort((u, v) => (u.id < v.id ? -1 : 1))) {
      const { A, B, C } = edgeGeom(e);
      const { len } = arcT(A, C, B, 0.5);
      for (const which of ['from', 'to']) {
        const txt = endText(e, which);
        if (!txt) continue;
        const w = Math.ceil(txt.length / 4) * 4 * LBL_CH, h = LBL_FS + 1;
        let best = null;
        for (let dist = NODE_R + 12; dist <= NODE_R + 12 + 12 * 10 && dist <= 0.45 * len; dist += 12) {
          for (const sd of [1, -1]) {
            const { t } = arcT(A, C, B, which === 'from' ? dist / len : 1 - dist / len);
            const pt = qpt(A, C, B, t), tg = qtan(A, C, B, t);
            const cx = pt[0] + -tg[1] * 8 * sd, cy = pt[1] + tg[0] * 8 * sd + 3;
            const box = { x0: cx - w / 2, x1: cx + w / 2, y0: cy - h + 1, y1: cy + 2 };
            const bad = placed.filter((q) => hit(box, q)).length + obst.filter((o) => hit(box, o)).length;
            if (!best || bad < best.bad) best = { bad, box, x: cx, y: cy };
            if (bad === 0) break;
          }
          if (best && best.bad === 0) break;
        }
        if (!best) {   // ветвь короче двух радиусов узла: подпись посередине, места нет
          const { t } = arcT(A, C, B, which === 'from' ? 0.3 : 0.7);
          const pt = qpt(A, C, B, t);
          best = { box: { x0: pt[0] - w / 2, x1: pt[0] + w / 2, y0: pt[1] - h, y1: pt[1] }, x: pt[0], y: pt[1] - 4 };
        }
        placed.push(best.box);
        labelPos[`${e.id}|${which}`] = { x: best.x, y: best.y };
      }
    }
  }
  const selEdge = sel && sel.type === 'edge' && view ? data.edges.find((e) => e.id === sel.id) : null;
  const selNode = sel && sel.type === 'node' && data ? data.nodes.find((n) => n.id === sel.id) : null;
  const arbVis = view ? view.edges.filter((e) => e.kind === 'arbitrageur' && Number.isFinite(e.emfBp)) : [];
  const maxEmf = arbVis.reduce((m, e) => Math.max(m, Math.abs(e.emfBp)), 0);
  const ageS = data && data.eventTimeMs ? Math.max(0, Math.round((now - data.eventTimeMs) / 1000)) : null;

  const arrow = (v, up = '▲', dn = '▼') => (v > 0 ? up : v < 0 ? dn : '');
  const recentChg = (key) => { const c = posChg.current[key]; return c && now - c.ts < CHG_MS ? c : null; };
  const lastChgTxt = (key) => { const c = posChg.current[key]; return c ? `${fmtQty(c.d)} ${c.unit}, ${Math.max(0, Math.round((now - c.ts) / 1000))} с назад` : 'с открытия страницы не менялась'; };
  const flows = view ? [...view.edges].filter((e) => Math.abs(e.flowUsdt) > 1e-6).sort((a, b) => Math.abs(b.flowUsdt) - Math.abs(a.flowUsdt)) : [];
  const flowTotal = flows.reduce((s, e) => s + Math.abs(e.flowUsdt), 0);
  // Масштаб анимации — относительно max(крупнейший поток, F_REF): иначе единственный микропоток (доли цента) выглядел бы
  // так же крупно, как поток в сотни USDT, и создавал впечатление движения капитала без изменения позиций.
  const F_REF = 5;
  const maxF = Math.max(F_REF, flows.reduce((m, e) => Math.max(m, Math.abs(e.flowUsdt)), 0));
  const fromTo = (e) => (e.flowUsdt >= 0 ? [e.from, e.to] : [e.to, e.from]);
  const Row = ({ k, v }) => <tr><td>{k}</td><td className="cg-mono">{v}</td></tr>;
  const edgeDetails = (e) => (
    <table className="cg-tbl"><tbody>
      <Row k="агент" v={e.agentId} />
      {e.kind === 'translator' && <Row k="источник якоря и наклона" v={e.source === 'live' ? `быстрая полоса (${e.levelsBid}/${e.levelsAsk} ур.)` : 'сегмент клиринга (20 ур.)'} />}
      <Row k="ветвь" v={`${e.from} → ${e.to}`} />
      {e.ends && <Row k={`позиция агента, ${e.ends.from.asset} (${e.from.split('@')[1]})`} v={`${fmtQty(e.ends.from.qty)} ${e.ends.from.asset}`} />}
      {e.ends && <Row k={`позиция агента, ${e.ends.to.asset} (${e.to.split('@')[1]})`} v={`${fmtQty(e.ends.to.qty)} ${e.ends.to.asset}`} />}
      {e.ends && <Row k={`  последнее изменение, ${e.ends.from.asset}`} v={lastChgTxt(`e|${e.id}|from`)} />}
      {e.ends && <Row k={`  последнее изменение, ${e.ends.to.asset}`} v={lastChgTxt(`e|${e.id}|to`)} />}
      {e.kind === 'translator' ? (<>
        <Row k={`якорь a (${e.quote} за ${e.base})`} v={fmt(e.anchor, 6)} />
        <Row k="сдвиг от позиции (skew)" v={`${sgn(-e.skewPm, 4)} ‰`} />
        <Row k="якорь после сдвига" v={fmt(e.anchorEff, 6)} />
        <Row k={`α_T, ${e.quote} за б.п.`} v={fmt(e.alphaT, 4)} />
        <Row k={`β_T = mid²/(10⁴·α_T), ${e.quote} за ед. ${e.base}`} v={fmt(e.betaT, 8)} />
        <Row k="r, б.п. на 1 млн USDT потока" v={fmt(e.rBp1M, 4)} />
        <Row k="позиция агента" v={`${sgn(e.positionUsdt, 0)} USDT`} />
        <Row k="поток за такт (направление from→to +)" v={`${sgn(e.flowUsdt, 3)} USDT`} />
      </>) : (<>
        <Row k="якорь σ*" v="0 (ребро-связка)" />
        <Row k="ЭДС: рассогласование цен узлов" v={`${sgn(e.emfBp, 3)} б.п.`} />
        <Row k="плечо α_A" v={`${fmt(e.alphaA, 3)} тыс.USDT/‰ (${fmt(e.alphaUsdt, 0)} USDT/б.п.)`} />
        <Row k="r, б.п. на 1 млн USDT потока" v={fmt(e.rBp1M, 4)} />
        <Row k="зона бездействия c" v={`${fmt(e.deadZonePm, 3)} ‰`} />
        <Row k={`нога ${e.src}`} v={`${sgn(e.legSrcUsdt, 0)} USDT`} />
        <Row k={`нога ${e.dst}`} v={`${sgn(e.legDstUsdt, 0)} USDT`} />
        <Row k="поток за такт (src→dst +)" v={`${sgn(e.flowUsdt, 3)} USDT`} />
      </>)}
    </tbody></table>
  );

  return (
    <div className="cg-page">
      <NavBar />
      <div className="cg-content">
        <div className="cg-head">
          <h2 className="cg-title">Цепь CE: активы — узлы, заявки агентов — ветви</h2>
          <div className="cg-desc">
            Узел = актив на бирже (цена в USDT). Ветвь = заявка агента: <b>переводчик</b> (внутри биржи, пара base/quote) или
            <b> арбитражёр</b> (один актив между биржами). У конца ветви, рядом с узлом, — позиция агента в валюте этого узла
            (в единицах валюты, минус — короткая). <b>В кружке узла</b>: сверху (белым) — суммарная позиция всех агентов на внутренней CE-бирже в валюте
            узла (сумма подписей у концов ветвей при узле; валюта узла указана слева по строке и цветом кольца); снизу (зелёным/красным) — нетто-позиция на этой
            внешней бирже, набранная хеджированием арбитражёров и переводчиков (по подтверждённым хедж-сделкам с момента сброса статистики). В подсказке — вклад каждого вида агентов.
            Шаг позиции за такт мал, поэтому изменение показывается на 15 с так: число в кружке окрашивается (зелёное — выросло, красное — упало; без изменений — белое сверху и серое снизу), а над кружком появляется рамка с ▲/▼ и ТОЛЬКО величиной шага («CE» — внутренняя биржа, «бирж.» — внешняя); у конца ветви рамка под подписью. Точное время изменения — в панели деталей.
            Значения — реальные: сегменты клиринга, позиции и дельты агентов (ledger), хедж-сделки. Белые точки бегут по ветви в сторону потока ценности последнего такта (быстрее и больше — крупнее поток относительно max(самого крупного потока, 5 $): микропоток в доли цента идёт медленно и не меняет позиции заметно); ветвь и узел вспыхивают, когда пересчёт изменил якорь, наклон или цену (▲▼ — направление изменения цены узла).
          </div>
        </div>
        {err && <div className="cg-err">{err}</div>}
        {!data && !err && <div className="cg-note">загрузка…</div>}
        {data && view && (<>
          <div className="cg-stats">
            <span className="cg-chip">узлов <b>{view.nodes.length}</b></span>
            <span className="cg-chip">ветвей <b>{view.edges.length}</b></span>
            <span className="cg-chip" title="наибольшее рассогласование цен узлов по видимым арбитражным ветвям">макс. |ЭДС| <b>{fmt(maxEmf, 2)}</b> б.п.</span>
            <span className="cg-chip">γ skew <b>{fmt(data.gamma, 4)}</b>, клэмп <b>{fmt(data.clampPm, 2)}</b>‰</span>
            <span className="cg-chip" title="Σ|поток| по видимым ветвям за последний такт клиринга">поток за такт <b>{fmtUsd(flowTotal)}</b> $</span>
            <span className={`cg-chip ${ageS != null && ageS > 60 ? 'cg-warn' : ''}`}>
              <i key={`b${beat}`} className="cg-beat" /> пересчёт {data.batchId ? <b>{String(data.batchId).slice(-6)}</b> : ''} · данные {ageS != null ? `${ageS} с назад` : '—'}
            </span>
            <span className="cg-chip">изменилось: ветвей <b>{Object.keys(chg.edges).filter((k) => view.edges.some((e) => e.id === k)).length}</b>, узлов <b>{Object.keys(chg.nodes).length}</b></span>
            <label className="cg-chip" title="Включает глубокий опрос стакана для видимых пар (аренда, гаснет сама). Якоря и наклоны переводчиков считаются по глубокому стакану ~каждые 2–3 с вместо ~25 с у клиринга.">
              <input type="checkbox" checked={liveOn} onChange={() => setLiveOn((v) => !v)} /> Быстрая полоса
            </label>
            {liveOn && lease && lease.ok && (
              <span className="cg-chip" title="Ветви с ● считаются по глубокому стакану, остальные — по данным клиринга">
                живых пар <b>{data.liveCount != null ? data.liveCount : 0}</b> из <b>{lease.requested}</b> · лимит аренд <b>{lease.max_items}</b>
                {lease.rejected_over_cap > 0 && <span className="cg-warn-t"> · {lease.rejected_over_cap} не принято (лимит) — отфильтруйте лишнее</span>}
              </span>
            )}
            {liveOn && lease && !lease.ok && <span className="cg-chip cg-warn">{lease.error}</span>}
            <label className="cg-chip">подписи ветвей:{' '}
              <select value={labels} onChange={(ev) => setLabels(ev.target.value)}>
                <option value="flow">поток</option>
                <option value="none">нет</option>
              </select>
            </label>
          </div>
          <div className="cg-layout">
            <div className="cg-graph">
              <svg viewBox={`0 0 ${W} ${H}`} className="cg-svg" role="img" aria-label="Граф CE">
                {view.vVis.map((v) => (
                  <text key={v} x={colX(v)} y={34} textAnchor="middle" className="cg-colhead" fill={vColor(v)}>{v}</text>
                ))}
                {view.aVis.map((a) => (
                  <text key={a} x={30} y={rowY(a) + 5} className="cg-rowhead" fill={aColor(a)}>{a}</text>
                ))}
                {view.edges.map((e) => {
                  const { A, B, C } = edgeGeom(e);
                  const col = edgeColor(e), on = selEdge && selEdge.id === e.id;
                  const f = Number(e.flowUsdt) || 0, c = chg.edges[e.id];
                  const d = `M${A[0]} ${A[1]}Q${C[0]} ${C[1]} ${B[0]} ${B[1]}`;
                  const dRev = `M${B[0]} ${B[1]}Q${C[0]} ${C[1]} ${A[0]} ${A[1]}`;
                  const mid = qpt(A, C, B, 0.5), tg = qtan(A, C, B, 0.5), nrm = [-tg[1], tg[0]];
                  const side = nrm[1] <= 0 ? 1 : -1;               // подпись всегда над линией
                  const lx = mid[0] + nrm[0] * 13 * side, ly = mid[1] + nrm[1] * 13 * side;
                  let l1 = '';
                  if (labels === 'flow' && Math.abs(f) > 1e-6) l1 = `${f >= 0 ? '→' : '←'} ${fmtUsd(f)} $`;
                  return (
                    <g key={e.id} className="cg-edge" onClick={() => setSel({ type: 'edge', id: e.id })}>
                      <path d={d} fill="none" stroke="transparent" strokeWidth={14} />
                      <path d={d} fill="none" stroke={col} opacity={Math.abs(f) > 1e-6 ? 1 : 0.55}
                        strokeWidth={on ? 3.8 : flowW(f, maxF)} strokeDasharray={e.kind === 'arbitrageur' ? '6 4' : undefined} strokeLinecap="round" />
                      {c && <path key={`p${chg.tick}`} d={d} fill="none" stroke={col} strokeLinecap="round" className="cg-pulse" />}
                      {Math.abs(f) > 1e-6 && Array.from({ length: flowDots(f, maxF) }).map((_, i) => {
                        const dur = flowDur(f, maxF), nd = flowDots(f, maxF);
                        return (
                          <circle key={`d${i}`} r={3.4} fill="#fff" className="cg-dot">
                            <animateMotion dur={`${dur}s`} begin={`${-i * dur / nd}s`} repeatCount="indefinite" path={f >= 0 ? d : dRev} />
                          </circle>
                        );
                      })}
                      {l1 && <text x={lx} y={ly} textAnchor="middle" className="cg-elbl" fill={col}>{l1}</text>}
                      {['from', 'to'].map((which) => {
                        const txt = endText(e, which), pt = labelPos[`${e.id}|${which}`];
                        if (!txt || !pt) return null;
                        const ch = recentChg(`e|${e.id}|${which}`);
                        return (
                          <g key={which}>
                            <text x={pt.x} y={pt.y} textAnchor="middle" className="cg-pos" fill={col}>{txt}</text>
                            {ch && <DeltaPill x={pt.x} y={pt.y + 12} d={ch.d} cls="cg-posd" />}
                          </g>
                        );
                      })}
                      <title>{`${e.agentId}: ${e.from} → ${e.to}`}</title>
                    </g>
                  );
                })}
                {view.nodes.map((n) => {
                  const [x, y] = [colX(n.venue), rowY(n.asset)];
                  const on = selNode && selNode.id === n.id;
                  const hp = n.hedgePos || null, cp = n.cePos || null;
                  const hch = hp ? recentChg(`n|${n.id}`) : null, cch = cp ? recentChg(`c|${n.id}`) : null;
                  const sgnCol = (v) => (v > 0 ? '#5fd08a' : v < 0 ? '#e6725a' : '#9fb3c4');
                  return (
                    <g key={n.id} className="cg-node" onClick={() => setSel({ type: 'node', id: n.id })}>
                      {chg.nodes[n.id] && <circle key={`n${chg.tick}`} cx={x} cy={y} r={NODE_R} fill="none" stroke={aColor(n.asset)} className="cg-npulse" />}
                      {cch && <circle key={`c${cch.ts}`} cx={x} cy={y} r={NODE_R} fill="none" stroke={sgnCol(cch.d)} className="cg-npulse" />}
                      {hch && <circle key={`h${hch.ts}`} cx={x} cy={y} r={NODE_R} fill="none" stroke={sgnCol(hch.d)} className="cg-npulse" />}
                      <circle cx={x} cy={y} r={NODE_R} fill="#0b0f14" stroke={on ? '#fff' : aColor(n.asset)} strokeWidth={on ? 3 : 2} />
                      <text x={x} y={y - 2} textAnchor="middle" className="cg-cepos" fill={cch ? sgnCol(cch.d) : '#e8eef4'}>{cp ? fmtQty(cp.qty) : '—'}</text>
                      <text x={x} y={y + 11} textAnchor="middle" className="cg-hpos" fill={hch ? sgnCol(hch.d) : '#9fb3c4'}>{hp ? fmtQty(hp.qty) : '—'}</text>
                      <text x={x} y={y + NODE_R + 14} textAnchor="middle" className="cg-price">{n.priceUsdt != null ? fmt(n.priceUsdt, n.priceUsdt < 1 ? 5 : 2) : '—'}{chg.nodes[n.id] ? ' ' + arrow(chg.nodes[n.id].dP) : ''}</text>
                      {Math.abs(n.netFlowUsdt) > 1e-6 && (
                        <text x={x} y={y + NODE_R + 26} textAnchor="middle" className="cg-net" fill={n.netFlowUsdt > 0 ? '#5fd08a' : '#e6725a'}>{n.netFlowUsdt > 0 ? '⇣' : '⇡'} {fmtUsd(n.netFlowUsdt)} $</text>
                      )}
                      {cch && <DeltaPill x={x} y={y - NODE_R - 6} tag="CE" d={cch.d} cls="cg-hposd" />}
                      {hch && <DeltaPill x={x} y={y - NODE_R - (cch ? 21 : 6)} tag="бирж." d={hch.d} cls="cg-hposd" />}
                      <title>{`${n.id}${cp ? `: позиции агентов на внутренней CE-бирже ${fmtQty(cp.qty)} ${n.asset} (переводчики ${fmtQty(cp.translator)}, арбитражёры ${fmtQty(cp.arbitrageur)})` : ''}${hp ? `; на внешней бирже от хеджирования ${fmtQty(hp.qty)} ${n.asset} (переводчики ${fmtQty(hp.translator)}, арбитражёры ${fmtQty(hp.arbitrageur)})` : ''}`}</title>
                    </g>
                  );
                })}
                {view.nodes.length === 0 && <text x={W / 2} y={H / 2} textAnchor="middle" className="cg-price">всё скрыто фильтрами</text>}
              </svg>
              <div className="cg-legend">
                <span><i style={{ borderColor: '#9aa7b4' }} /> переводчик (цвет — биржа)</span>
                <span><i style={{ borderColor: '#9aa7b4', borderTopStyle: 'dashed' }} /> арбитражёр (цвет — актив)</span>
                <span>Клик по ветви или узлу — подробности.</span>
              </div>
            </div>
            <div className="cg-side">
              <div className="cg-box">
                <div className="cg-boxh">Биржи</div>
                {data.venues.map((v) => (
                  <label key={v} className="cg-chk"><input type="checkbox" checked={!hidden.venues[v]} onChange={() => toggle('venues', v)} />
                    <span style={{ color: vColor(v) }}>{v}</span></label>
                ))}
                <div className="cg-boxh">Валюты (активы)</div>
                {view.assets.map((a) => (
                  <label key={a} className="cg-chk"><input type="checkbox" checked={!hidden.assets[a]} onChange={() => toggle('assets', a)} />
                    <span style={{ color: aColor(a) }}>{a}</span></label>
                ))}
                <div className="cg-boxh">Пары переводчиков</div>
                {data.pairs.map((p) => (
                  <label key={p} className="cg-chk"><input type="checkbox" checked={!hidden.pairs[p]} onChange={() => toggle('pairs', p)} />{p}</label>
                ))}
                <div className="cg-boxh">Агенты</div>
                <label className="cg-chk"><input type="checkbox" checked={!hidden.kinds.translator} onChange={() => toggle('kinds', 'translator')} />переводчики</label>
                <label className="cg-chk"><input type="checkbox" checked={!hidden.kinds.arbitrageur} onChange={() => toggle('kinds', 'arbitrageur')} />арбитражёры</label>
              </div>
              <div className="cg-box">
                <div className="cg-boxh">Куда идёт капитал (за такт, USDT)</div>
                {flows.length === 0 && <div className="cg-note">потоков в последнем такте нет</div>}
                {flows.slice(0, 8).map((e) => {
                  const [fa, fb] = fromTo(e);
                  return (
                    <div key={e.id} className="cg-flow" onClick={() => setSel({ type: 'edge', id: e.id })}>
                      <span className="cg-mono">{fa} → {fb}</span>
                      <b style={{ color: e.kind === 'translator' ? vColor(e.venue) : aColor(e.asset) }}>{fmtUsd(e.flowUsdt)}</b>
                    </div>
                  );
                })}
              </div>
              <div className="cg-box">
                <div className="cg-boxh">{selEdge ? 'Ветвь' : selNode ? 'Узел' : 'Подробности'}</div>
                {selEdge && edgeDetails(selEdge)}
                {selNode && (
                  <table className="cg-tbl"><tbody>
                    <Row k="узел" v={selNode.id} />
                    <Row k="цена актива" v={selNode.priceUsdt != null ? `${fmt(selNode.priceUsdt, 6)} USDT` : '—'} />
                    <Row k="ветвей при узле (видимых)" v={view.edges.filter((e) => e.from === selNode.id || e.to === selNode.id).length} />
                    {selNode.cePos && <Row k="позиции агентов на внутренней CE-бирже" v={`${fmtQty(selNode.cePos.qty)} ${selNode.asset}`} />}
                    {selNode.cePos && <Row k="  из них переводчики" v={`${fmtQty(selNode.cePos.translator)} ${selNode.asset}`} />}
                    {selNode.cePos && <Row k="  из них арбитражёры" v={`${fmtQty(selNode.cePos.arbitrageur)} ${selNode.asset}`} />}
                    {selNode.cePos && <Row k="  последнее изменение" v={lastChgTxt(`c|${selNode.id}`)} />}
                    {selNode.hedgePos && <Row k="позиция на внешней бирже от хеджирования (нетто)" v={`${fmtQty(selNode.hedgePos.qty)} ${selNode.asset}`} />}
                    {selNode.hedgePos && <Row k="  из них переводчики" v={`${fmtQty(selNode.hedgePos.translator)} ${selNode.asset}`} />}
                    {selNode.hedgePos && <Row k="  из них арбитражёры" v={`${fmtQty(selNode.hedgePos.arbitrageur)} ${selNode.asset}`} />}
                    {selNode.hedgePos && <Row k="  последнее изменение" v={lastChgTxt(`n|${selNode.id}`)} />}
                    {data.hedgeSinceMs > 0 && <Row k="хедж-сделки учтены с" v={new Date(data.hedgeSinceMs).toLocaleString('ru-RU')} />}
                  </tbody></table>
                )}
                {!selEdge && !selNode && <div className="cg-note">Выберите ветвь или узел на графе.</div>}
              </div>
            </div>
          </div>
        </>)}
        {isAuth && <CeClearingPanel />}
      </div>
    </div>
  );
};

export default CeGraphLive;
