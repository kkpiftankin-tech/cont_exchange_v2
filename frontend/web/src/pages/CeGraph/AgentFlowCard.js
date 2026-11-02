import React from 'react';
import { vColor, aColor, fmt, sgn, fmtQty, fmtQtyAbs, fmtUsd, flowU, flowDur } from './ceGraphUtil';
import './AgentFlowCard.css';

// Карточка одного агента вкладки «Граф» (раздел «Агенты», режим «Добавить»): узлы CE — агент — внешняя биржа, рёбра — валюты
// (стрелка от агента к узлу = агент отдаёт валюту, к агенту = получает), толщина — стоимость потока в USDT, точки бегут по
// потоку, подпись — «глагол + количество за такт» и позиция. Чистый рендер: количества, знаки и хедж-позиции агента посчитаны
// в BFF (ce_graph_build.js → edge.agentView, CALC-CE-GRAPH), здесь только раскладка.
//
// Переводчик: четыре ребра (база и quote к CE; база и quote к внешней бирже). CE-рёбра — поток последнего такта клиринга и позиция
// агента (edge.ends); рёбра внешней биржи — собственная хедж-позиция агента и последний шаг её изменения (по сравнению опросов
// страницы: хедж-заполнения приходят не тактами, а заявками, поэтому «потока за такт» у них нет).
// Арбитражёр: два ребра — актив у узла биржи-источника и у узла биржи-приёмника; хедж агента (если был) — строкой под рисунком.
const VB_W = 800, VB_Y = 34, VB_H = 138, CY = 100, NODE_H = 112;   // рисунок обрезан по подписям: узлы 44…156, подписи 50…160
const NX = { ce: 90, ag: 400, ex: 710 }, NW = { ce: 150, ag: 190, ex: 150 };
const LIVE_USD = 1e-6;
const pipeW = (usd, maxF) => 2.5 + 7 * Math.sqrt(flowU(usd, maxF));

// Труба между краем агента (x1) и краем внешнего узла (x2). dir: 'out' — агент → узел, 'in' — узел → агент, null — потока нет.
const Pipe = ({ x1, x2, y, col, usd, maxF, dir, title }) => {
  if (!dir) return <g className="ac-pipe"><title>{title}</title><line className="ac-idle" x1={x1} x2={x2} y1={y} y2={y} /></g>;
  const sw = pipeW(usd, maxF), sx = dir === 'out' ? x1 : x2, ex = dir === 'out' ? x2 : x1, s = Math.sign(ex - sx);
  const AL = 8 + 1.2 * sw, AW = 4 + 0.8 * sw, pad = 5;
  const a = sx + s * pad, tip = ex - s * pad, base = tip - s * AL;
  const d = `M${a} ${y}L${base + s * AL * 0.35} ${y}`;
  return (
    <g className="ac-pipe">
      <title>{title}</title>
      <path d={d} fill="none" stroke={col} strokeWidth={sw} strokeLinecap="round" />
      <path className="ac-beads" d={d} fill="none" style={{ strokeWidth: Math.max(1.2, sw * 0.42), animationDuration: `${flowDur(usd, maxF).toFixed(2)}s` }} />
      <polygon points={`${tip},${y} ${base},${y - AW} ${base},${y + AW}`} fill={col} />
    </g>
  );
};

// Подпись ребра: первая строка (ближе к трубе) — что делает агент, вторая — позиция. above — подписи над трубой, иначе под ней.
const Lbl = ({ x, y, sw, above, verb, num, cur, sub, warn }) => {
  const y1 = above ? y - sw / 2 - 6 : y + sw / 2 + 13, y2 = above ? y1 - 13 : y1 + 13;
  return (
    <g>
      <text className="ac-lbl" x={x} y={y1} textAnchor="middle">{verb ? `${verb} ` : ''}<tspan className="ac-v">{num}</tspan>{cur ? ` ${cur}` : ''}</text>
      {sub && <text className={`ac-lbl2${warn ? ' ac-low' : ''}`} x={x} y={y2} textAnchor="middle">{sub}</text>}
    </g>
  );
};

const Node = ({ x, w, kind, stroke, lines }) => {
  const fs = (l) => l.size || (l.cls === 'ac-t1' ? 13 : 11);
  const tot = lines.reduce((s, l) => s + fs(l) * 1.3, 0);
  let y = CY - tot / 2;
  return (
    <g className={`ac-node ac-node-${kind}`}>
      <rect className="ac-box" x={x - w / 2} y={CY - NODE_H / 2} width={w} height={NODE_H} rx={12} style={stroke ? { stroke } : undefined} />
      {lines.map((l, i) => {
        y += fs(l) * 1.3;
        return <text key={i} className={l.cls} x={x} y={(y - fs(l) * 0.3).toFixed(1)} textAnchor="middle" style={{ fontSize: fs(l), ...(l.fill ? { fill: l.fill } : null) }}>{l.t}</text>;
      })}
    </g>
  );
};

const AgentFlowCard = ({ edge, maxF, recent, onRemove }) => {
  const tr = edge.kind === 'translator';
  const av = edge.agentView || null, fq = (av && av.flowQty) || { from: null, to: null }, hedge = (av && av.hedge) || [];
  const liveFlow = Math.abs(Number(edge.flowUsdt)) > LIVE_USD;
  const posOf = (k) => (edge.ends && edge.ends[k] ? edge.ends[k].qty : null);
  const posTxt = (k) => (edge.ends ? `позиция ${fmtQty(posOf(k))}` : 'позиции нет');

  // ребро к узлу CE: k — конец ветви (translator: from = quote, to = base; arbitrageur: from = src, to = dst)
  const ceLeg = (k, asset, verbs) => {
    const amt = fq[k], known = Number.isFinite(amt);
    const live = liveFlow && known && amt !== 0;
    const dir = live ? (amt > 0 ? 'out' : 'in') : null;
    return { dir, usd: Math.abs(Number(edge.flowUsdt)) || 0, asset,
      verb: live ? (amt > 0 ? verbs[0] : verbs[1]) : '', num: !known ? '—' : live ? fmtQtyAbs(amt) : '0', sub: posTxt(k),
      title: `${edge.agentId}: ${live ? `${amt > 0 ? verbs[0] : verbs[1]} ${fmtQtyAbs(amt)} ${asset} за такт` : `потока ${asset} нет`}; ${posTxt(k)}` };
  };
  // ребро к внешней бирже: собственная хедж-позиция агента и её последний шаг (15 с)
  const hedgeLeg = (asset, verbs) => {
    const h = hedge.find((x) => x.venue === edge.venue && x.asset === asset), qty = h ? h.qty : 0;
    const ch = recent(`h|${edge.id}|${edge.venue}|${asset}`);
    const dir = ch ? (ch.d > 0 ? 'in' : 'out') : null;
    return { dir, usd: ch && h && h.px > 0 ? Math.abs(ch.d) * h.px : 0, asset,
      verb: ch ? (ch.d > 0 ? verbs[0] : verbs[1]) : '', num: ch ? fmtQtyAbs(ch.d) : 'новых хеджей нет', sub: `хедж-позиция ${fmtQty(qty)}`,
      title: `${edge.agentId}: ${ch ? `${ch.d > 0 ? verbs[0] : verbs[1]} ${fmtQtyAbs(ch.d)} ${asset} на ${edge.venue}` : `новых хедж-сделок ${asset} нет`}; хедж-позиция агента ${fmtQty(qty)} ${asset}` };
  };

  const gapL = [NX.ce + NW.ce / 2, NX.ag - NW.ag / 2], gapR = [NX.ag + NW.ag / 2, NX.ex - NW.ex / 2];
  const midL = (gapL[0] + gapL[1]) / 2, midR = (gapR[0] + gapR[1]) / 2;
  const Y_TOP = CY - 24, Y_BOT = CY + 24;

  let nodes, pipes;
  if (tr) {
    const legs = {
      ceBase: ceLeg('to', edge.base, ['продаёт', 'покупает']), ceQuote: ceLeg('from', edge.quote, ['платит', 'получает']),
      exBase: hedgeLeg(edge.base, ['купил', 'продал']), exQuote: hedgeLeg(edge.quote, ['получил', 'заплатил']),
    };
    const venueCol = vColor(edge.venue);
    nodes = (<>
      <Node x={NX.ce} w={NW.ce} kind="ce" lines={[{ t: 'Внутренняя биржа CE', cls: 'ac-t1', size: 12.5 }, { t: `пара ${edge.pair}`, cls: 'ac-t2' }]} />
      <Node x={NX.ag} w={NW.ag} kind="ag" stroke={venueCol} lines={[
        { t: `Переводчик ${edge.pair}`, cls: 'ac-t1' }, { t: edge.agentId, cls: 'ac-t3' },
        { t: `сдвиг якоря ${sgn(-edge.skewPm, 3)} ‰`, cls: 'ac-t2' }, { t: `источник: ${edge.source === 'live' ? 'быстрая полоса' : 'клиринг'}`, cls: 'ac-t2' }]} />
      <Node x={NX.ex} w={NW.ex} kind="ex" stroke={venueCol} lines={[{ t: edge.venue, cls: 'ac-t1', fill: venueCol }, { t: 'внешняя биржа', cls: 'ac-t2' }, { t: `мид ${fmt(edge.anchor, 6)}`, cls: 'ac-t3' }]} />
    </>);
    const P = (l, x1, x2, y, mid, above) => {
      const sw = l.dir ? pipeW(l.usd, maxF) : 1.4;
      return (<g key={`${mid}${y}`}>
        <Pipe x1={x1} x2={x2} y={y} col={aColor(l.asset)} usd={l.usd} maxF={maxF} dir={l.dir} title={l.title} />
        <Lbl x={mid} y={y} sw={sw} above={above} verb={l.verb} num={l.num} cur={l.dir || l.num === '0' ? l.asset : ''} sub={l.sub} />
      </g>);
    };
    pipes = (<>
      {P(legs.ceBase, gapL[1], gapL[0], Y_TOP, midL, true)}{P(legs.ceQuote, gapL[1], gapL[0], Y_BOT, midL, false)}
      {P(legs.exBase, gapR[0], gapR[1], Y_TOP, midR, true)}{P(legs.exQuote, gapR[0], gapR[1], Y_BOT, midR, false)}
    </>);
  } else {
    const L = ceLeg('from', edge.asset, ['отдаёт', 'получает']), R = ceLeg('to', edge.asset, ['отдаёт', 'получает']);
    const accent = aColor(edge.asset);
    nodes = (<>
      <Node x={NX.ce} w={NW.ce} kind="ce" lines={[{ t: 'Узел CE', cls: 'ac-t1' }, { t: edge.from, cls: 'ac-t1', fill: vColor(edge.src), size: 12 }, { t: 'биржа-источник', cls: 'ac-t2' }]} />
      <Node x={NX.ag} w={NW.ag} kind="ag" stroke={accent} lines={[
        { t: `Арбитражёр ${edge.asset}`, cls: 'ac-t1' }, { t: edge.agentId, cls: 'ac-t3' },
        { t: `ЭДС ${Number.isFinite(edge.emfBp) ? sgn(edge.emfBp, 2) : '—'} б.п.`, cls: 'ac-t2' }, { t: `зона c ${Number.isFinite(edge.deadZonePm) ? fmt(edge.deadZonePm, 3) : '—'} ‰`, cls: 'ac-t2' }]} />
      <Node x={NX.ex} w={NW.ex} kind="ce" lines={[{ t: 'Узел CE', cls: 'ac-t1' }, { t: edge.to, cls: 'ac-t1', fill: vColor(edge.dst), size: 12 }, { t: 'биржа-приёмник', cls: 'ac-t2' }]} />
    </>);
    const P = (l, x1, x2, mid) => {
      const sw = l.dir ? pipeW(l.usd, maxF) : 1.4;
      return (<g key={mid}>
        <Pipe x1={x1} x2={x2} y={CY} col={accent} usd={l.usd} maxF={maxF} dir={l.dir} title={l.title} />
        <Lbl x={mid} y={CY} sw={sw} above verb={l.verb} num={l.num} cur={l.dir || l.num === '0' ? l.asset : ''} sub={l.sub} />
      </g>);
    };
    pipes = (<>{P(L, gapL[1], gapL[0], midL)}{P(R, gapR[0], gapR[1], midR)}</>);
  }

  const hedgeTxt = tr ? null : hedge.map((h) => `${h.venue}: ${h.asset} ${fmtQty(h.qty)}`).join(' · ');
  return (
    <div className="ac-card" data-agent={edge.agentId}>
      <div className="ac-head">
        <b className="ac-id" style={{ color: tr ? vColor(edge.venue) : aColor(edge.asset) }}>{edge.agentId}</b>
        <span className="ac-kind">{tr ? `переводчик · ${edge.pair} · ${edge.venue}` : `арбитражёр · ${edge.asset} · ${edge.src} → ${edge.dst}`}</span>
        <button type="button" className="ac-x" aria-label={`Убрать агента ${edge.agentId}`} title="Убрать с экрана" onClick={onRemove}>×</button>
      </div>
      <svg viewBox={`0 ${VB_Y} ${VB_W} ${VB_H}`} className="ac-svg" role="img" aria-label={`Агент ${edge.agentId}: потоки валют между внутренней биржей CE и ${tr ? edge.venue : 'узлами бирж'}`}>
        {nodes}
        {pipes}
      </svg>
      <div className="ac-foot">
        <span>поток за такт <b>{liveFlow ? `${edge.flowUsdt > 0 ? '→' : '←'} ${fmtUsd(edge.flowUsdt)} $` : '0'}</b></span>
        {!edge.ends && <span className="ac-warn">агента нет в ledger — позиций пока нет</span>}
        {!av && <span className="ac-warn">BFF не отдал данные карточки — обновите frontend-api</span>}
        {hedgeTxt && <span>хедж агента: {hedgeTxt}</span>}
      </div>
    </div>
  );
};

export default AgentFlowCard;
