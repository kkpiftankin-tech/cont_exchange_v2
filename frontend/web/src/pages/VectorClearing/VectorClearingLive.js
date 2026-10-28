import React, { useEffect, useState, useCallback } from 'react';
import { createPortal } from 'react-dom';
import NavBar from '../../components/NavBar';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import { groupTranslators, groupArbitrageurs, isIdleAgent, mergeStableRows } from './agentRowsLayout';
import AgentPnlChart from './AgentPnlChart';
import SystemPnlBlock from './SystemPnlBlock';
import './VectorClearingLive.css';

// F-05A: живой мониторинг векторного клиринга (ClickHouse vector_clearing_results).
// Ops-страница: market_data векторизует внешнюю ликвидность → matching решает
// QP Wx=0 (OSQP) + surplus → результат сюда.
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 3000;

// Стабильная палитра для per-venue кривых ликвидности (известные площадки + фолбэк).
const VENUE_COLORS = {
  binance: '#f0b90b', okx: '#7a9cff', kraken: '#8f5be6',
  coinbase: '#2a7de1', uniswap_v3: '#ff2fa0', bybit: '#ff8a3d',
};
const VENUE_PALETTE = ['#f0b90b', '#7a9cff', '#8f5be6', '#2a7de1', '#ff2fa0', '#ff8a3d', '#5fd0a0', '#e06060'];
const venueColor = (venue, i) => VENUE_COLORS[venue] || VENUE_PALETTE[i % VENUE_PALETTE.length];

// Компактное число: крупные — 2 знака, средние — 4, мелкие — значащие.
function fmtNum(v) {
  const n = Number(v);
  if (!Number.isFinite(n)) return v;
  const a = Math.abs(n);
  if (a === 0) return '0';
  if (a >= 100) return n.toFixed(2);
  if (a >= 1) return n.toFixed(4);
  if (a >= 0.0001) return n.toFixed(6);
  return n.toExponential(3);
}

function fmtTime(ms) {
  const n = Number(ms);
  if (!Number.isFinite(n)) return '—';
  try {
    return new Date(n).toLocaleTimeString('ru-RU', { hour12: false });
  } catch (e) {
    return String(ms);
  }
}

// График ликвидности венью. ВСЕ вычисления (raw cumulative, VWAP, α_ext/α_T/β_T,
// safe-линия) — на backend (ADR-053, endpoint /vector-clearing/curve). Этот
// компонент — ЧИСТЫЙ РЕНДЕР: запрашивает готовые массивы {q, price} по venue/symbol
// и контролам, при смене контролов перезапрашивает, рисует. Никаких вычислений кривой.
// Ориентация осей: 'vp' объём→цена (график 02), 'pv' цена→объём (график 01).
function LiquidityChart({ venue, symbol, ts, onPauseChange }) {
  const [nBuy, setNBuy] = useState('');        // '' → backend берёт все уровни
  const [nSell, setNSell] = useState('');
  const [orient, setOrient] = useState('vp');
  const [anchorMode, setAnchorMode] = useState('mid');
  const [theta, setTheta] = useState('0.60');
  const [showRaw, setShowRaw] = useState(true);
  const [showVwap, setShowVwap] = useState(true);
  const [showSafe, setShowSafe] = useState(true);
  const [showCe, setShowCe] = useState(true);   // CE-кривая: зона комиссии/бездействия
  const [showFob, setShowFob] = useState(false);
  const [showAggAll, setShowAggAll] = useState(false);  // #3a: агрегат всех площадок пары
  const [showAggEx, setShowAggEx] = useState(false);    // #3b: все площадки КРОМЕ этой
  const [showMethods, setShowMethods] = useState(false); // §6.2: 4 кривые наклона M1..M4
  const [showPerVenue, setShowPerVenue] = useState(false); // кривые ликвидности всех площадок пары
  const [paused, setPaused] = useState(false);  // СТОП: заморозить данные (без мигания/исчезновения)
  const [zoom, setZoom] = useState(1);           // масштаб графика (увеличение во время паузы)
  const [data, setData] = useState(null);
  const [err, setErr] = useState('');
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    // Пауза: не перезапрашиваем и не трогаем data — график заморожен, не мигает и не
    // «дёргается» при смене ts от родителя. Чекбоксы/зум работают от замороженных данных.
    if (paused) return;
    let alive = true;
    setLoading(true); setErr('');
    const params = { venue, symbol, anchor: anchorMode, theta };
    if (ts != null) params.ts = ts;
    if (nBuy) params.nBuy = nBuy;
    if (nSell) params.nSell = nSell;
    axios.get(`${API_BASE}/vector-clearing/curve`, { params, timeout: 10000 })
      .then((r) => { if (alive) setData(r.data); })
      .catch((e) => { if (alive) setErr(e.message || 'ошибка загрузки кривой'); })
      .finally(() => { if (alive) setLoading(false); });
    return () => { alive = false; };
  }, [venue, symbol, ts, nBuy, nSell, anchorMode, theta, paused]);

  // Сообщаем родителю о паузе: он замораживает опрос списка батчей, чтобы строки не
  // двигались и открытый батч не исчезал через 20-30с. При размонтировании (закрыли
  // строку) снимаем заморозку.
  useEffect(() => { if (onPauseChange) onPauseChange(paused); }, [paused, onPauseChange]);
  useEffect(() => () => { if (onPauseChange) onPauseChange(false); }, [onPauseChange]);

  // Ошибку показываем только НЕ на паузе и когда нет замороженных данных (иначе график
  // не должен исчезать во время паузы из-за разового сбоя запроса).
  if (err && !paused && !data) return <div className="vc-error">Ошибка: {err}</div>;
  if (!data) return <div className="vc-note">{loading ? 'загрузка кривой…' : 'нет данных кривой'}</div>;

  // Готовые серии от backend (signed q, price). Тумблеры только скрывают/показывают.
  const vp = orient === 'vp';
  const rawBid = showRaw ? (data.rawBid || []) : [];
  const rawAsk = showRaw ? (data.rawAsk || []) : [];
  const vwapBid = showVwap ? (data.vwapBid || []) : [];
  const vwapAsk = showVwap ? (data.vwapAsk || []) : [];
  const safe = showSafe ? (data.safe || []) : [];   // safe-кривая в ОБЕИХ ориентациях
  // CE-кривая = две линейные стороны с разрывом (no-trade зона спред+комиссия), без вертикали.
  const ceSell = showCe ? (data.ceSell || []) : [];
  const ceBuy = showCe ? (data.ceBuy || []) : [];
  const deadLow = Number(data.deadLow), deadHigh = Number(data.deadHigh);
  const deadZonePm = Number(data.deadZonePm);
  const fobBid = showFob ? (data.fobBid || []) : [];
  const fobAsk = showFob ? (data.fobAsk || []) : [];
  // #3: агрегатные линейные кривые ликвидности (backend считает сумму по площадкам).
  const aggAll = showAggAll ? (data.aggAll || []) : [];
  const aggEx = showAggEx ? (data.aggEx || []) : [];
  // §6.2: 4 кривые наклона (СЫРОЙ β, θ=1 — сравнимы с VWAP), из движка (single source).
  const sm = data.safeMethods || {};
  const mTangent = showMethods ? (sm.tangent || []) : [];
  const mLsq = showMethods ? (sm.lsq || []) : [];
  const mProfit = showMethods ? (sm.profit || []) : [];
  const mMinorant = showMethods ? (sm.minorant || []) : [];
  const selMethod = Number(data.selectedSlopeMethod) || 4;
  const hasMethods = !!(sm.tangent || sm.lsq || sm.profit || sm.minorant);
  // Индивидуальные линейные кривые ликвидности КАЖДОЙ площадки по этой паре (backend).
  const perVenue = showPerVenue ? (data.perVenue || []) : [];
  const hasPerVenue = !!((data.perVenue || []).length);
  const anchor = Number(data.anchor);
  const bestBid = Number(data.bestBid), bestAsk = Number(data.bestAsk);
  const eng = data.engine || null;

  const all = [...rawBid, ...rawAsk, ...vwapBid, ...vwapAsk, ...safe, ...ceSell, ...ceBuy, ...fobBid, ...fobAsk, ...aggAll, ...aggEx, ...mTangent, ...mLsq, ...mProfit, ...mMinorant, ...perVenue.flatMap((v) => v.curve || [])];
  if (!all.length) return <div className="vc-note">нет точек для графика</div>;

  // Масштабирование осей — это ОТРИСОВКА (не вычисление кривой).
  const qs = all.map((p) => p.q).concat([0]);
  const ps = all.map((p) => p.price);
  let vMin = Math.min(...qs), vMax = Math.max(...qs);
  let pMin = Math.min(...ps), pMax = Math.max(...ps);
  const vPad = (vMax - vMin) * 0.05 || 1; vMin -= vPad; vMax += vPad;
  const pPad = (pMax - pMin) * 0.08 || 1; pMin -= pPad; pMax += pPad;
  const xMin = vp ? vMin : pMin, xMax = vp ? vMax : pMax;
  const yMin = vp ? pMin : vMin, yMax = vp ? pMax : vMax;

  // Зум: масштабируем полотно (увеличение во время паузы). Поля фиксированы ⇒ растёт
  // область построения. Клэмп 1..3.5.
  const zc = Math.max(1, Math.min(3.5, zoom));
  const W = Math.round(620 * zc), H = Math.round(300 * zc), ml = 66, mr = 14, mt = 14, mb = 46;
  const pw = W - ml - mr, ph = H - mt - mb;
  const X = (v) => ml + (xMax === xMin ? pw / 2 : (v - xMin) / (xMax - xMin) * pw);
  const Y = (v) => mt + (yMax === yMin ? ph / 2 : (yMax - v) / (yMax - yMin) * ph);
  const xv = vp ? ((p) => p.q) : ((p) => p.price);
  const yv = vp ? ((p) => p.price) : ((p) => p.q);
  const line = (pts) => pts.map((p) => `${X(xv(p))},${Y(yv(p))}`).join(' ');
  const fmt = (v) => { const a = Math.abs(v); if (!Number.isFinite(v)) return '—'; if (a === 0) return '0'; if (a >= 1000) return v.toFixed(0); if (a >= 1) return v.toFixed(2); return v.toFixed(4); };
  const fmtSig = (v) => (Number.isFinite(Number(v)) ? (Math.abs(Number(v)) >= 100 ? Number(v).toFixed(1) : Number(v).toPrecision(4)) : '—');

  const priceGuide = (val, stroke, dash, key) => (vp
    ? <line key={key} x1={ml} y1={Y(val)} x2={ml + pw} y2={Y(val)} stroke={stroke} strokeDasharray={dash} />
    : <line key={key} x1={X(val)} y1={mt} x2={X(val)} y2={mt + ph} stroke={stroke} strokeDasharray={dash} />);
  const volGuide = (val, stroke, dash, key) => (vp
    ? <line key={key} x1={X(val)} y1={mt} x2={X(val)} y2={mt + ph} stroke={stroke} strokeDasharray={dash} />
    : <line key={key} x1={ml} y1={Y(val)} x2={ml + pw} y2={Y(val)} stroke={stroke} strokeDasharray={dash} />);

  const xTitle = vp ? 'объём base (знаковый): − продажа в bid / + покупка из ask' : 'цена (quote за base)';
  const yTitle = vp ? 'цена (quote за base)' : 'объём base (знаковый)';

  // Лесенка стакана «как на binance»: явные цена/объём на уровень + накопленный + полоски
  // глубины. Отдельно от кривой ликвидности (которая плохо читается при узком спреде).
  const fmtPrice = (p) => {
    const a = Math.abs(Number(p));
    if (!Number.isFinite(a) || a === 0) return '—';
    if (a >= 1000) return Number(p).toFixed(2);
    if (a >= 1) return Number(p).toFixed(4);
    return Number(p).toPrecision(6);
  };
  const fmtSize = (v) => {
    const a = Math.abs(Number(v));
    if (!Number.isFinite(a) || a === 0) return '0';
    if (a >= 1) return Number(v).toFixed(3);
    if (a >= 0.001) return Number(v).toFixed(4);
    return Number(v).toExponential(1);
  };
  const book = data.book || null;
  const N_LADDER = 15;
  const ladder = (() => {
    if (!book || (!(book.asks || []).length && !(book.bids || []).length)) return null;
    const asks = (book.asks || []);   // asc по цене (лучший первый)
    const bids = (book.bids || []);   // desc по цене (лучший первый)
    const maxCum = Math.max(
      asks.length ? asks[Math.min(asks.length, N_LADDER) - 1].cum : 0,
      bids.length ? bids[Math.min(bids.length, N_LADDER) - 1].cum : 0, 1e-12);
    const asksShow = asks.slice(0, N_LADDER).reverse();  // высшая цена сверху, лучший аск снизу у спреда
    const bidsShow = bids.slice(0, N_LADDER);            // лучший бид сверху у спреда
    const spreadAbs = (bestAsk > 0 && bestBid > 0) ? (bestAsk - bestBid) : null;
    const spreadBps = (bestAsk > 0 && bestBid > 0) ? ((bestAsk / bestBid - 1) * 10000) : null;
    const row = (lvl, side) => (
      <div className="vc-ob-row" key={side + lvl.price}>
        <div className={side === 'ask' ? 'vc-ob-bar vc-ob-bar-ask' : 'vc-ob-bar vc-ob-bar-bid'}
          style={{ width: `${Math.min(100, 100 * lvl.cum / maxCum)}%` }} />
        <span className={side === 'ask' ? 'vc-ob-price vc-ob-ask' : 'vc-ob-price vc-ob-bid'}>{fmtPrice(lvl.price)}</span>
        <span className="vc-ob-size">{fmtSize(lvl.size)}</span>
        <span className="vc-ob-cum">{fmtSize(lvl.cum)}</span>
      </div>
    );
    return (
      <div className="vc-orderbook" onClick={(e) => e.stopPropagation()}>
        <div className="vc-ob-head"><span>цена</span><span>объём</span><span>Σ объём</span></div>
        <div className="vc-ob-asks">{asksShow.map((l) => row(l, 'ask'))}</div>
        <div className="vc-ob-spread">
          спред {spreadAbs != null ? fmtPrice(spreadAbs) : '—'}
          {spreadBps != null ? ` (${spreadBps.toFixed(2)} bps)` : ''}
          <span className="vc-ob-mid"> · мид {fmtPrice((bestBid + bestAsk) / 2)}</span>
        </div>
        <div className="vc-ob-bids">{bidsShow.map((l) => row(l, 'bid'))}</div>
      </div>
    );
  })();

  // Депт-график «как на binance» с НОРМАЛЬНЫМИ шкалами: ось X — цена, ось Y —
  // накопленный объём. Аски вверх (+), биды ВНИЗ (−, перевёрнуты по оси объёма,
  // по просьбе владельца). Реальный стакан (не модельная кривая).
  const depthChart = (() => {
    if (!book) return null;
    const asks = book.asks || [], bids = book.bids || [];
    if (!asks.length || !bids.length) return null;
    const pLoRaw = bids[bids.length - 1].price, pHiRaw = asks[asks.length - 1].price;
    const pSpan = (pHiRaw - pLoRaw) || Math.max(1, pHiRaw * 1e-4);
    const pMin = pLoRaw - pSpan * 0.03, pMax = pHiRaw + pSpan * 0.03;
    const maxAsk = asks[asks.length - 1].cum, maxBid = bids[bids.length - 1].cum;
    const vTop = Math.max(maxAsk, 1e-9) * 1.10, vBot = -Math.max(maxBid, 1e-9) * 1.10;
    const W = 640, H = 320, ml = 70, mr = 14, mt = 14, mb = 34, pw = W - ml - mr, ph = H - mt - mb;
    const X = (p) => ml + (p - pMin) / (pMax - pMin) * pw;
    const Y = (v) => mt + (vTop - v) / (vTop - vBot) * ph;
    const bestAsk = asks[0].price, bestBid = bids[0].price, mid = (bestAsk + bestBid) / 2;
    // Площадь асков: от (bestAsk,0) ступенями вверх-вправо; биды — вниз-влево (−cum).
    let aLine = `${X(bestAsk)},${Y(0)}`, pv = 0;
    for (const l of asks) { aLine += ` ${X(l.price)},${Y(pv)} ${X(l.price)},${Y(l.cum)}`; pv = l.cum; }
    const aFill = `${aLine} ${X(asks[asks.length - 1].price)},${Y(0)}`;
    let bLine = `${X(bestBid)},${Y(0)}`; pv = 0;
    for (const l of bids) { bLine += ` ${X(l.price)},${Y(-pv)} ${X(l.price)},${Y(-l.cum)}`; pv = l.cum; }
    const bFill = `${bLine} ${X(bids[bids.length - 1].price)},${Y(0)}`;
    const xticks = [0, 0.25, 0.5, 0.75, 1].map((f) => pMin + (pMax - pMin) * f);
    const yticks = [];
    for (const f of [0.5, 1]) { yticks.push(maxAsk * f); yticks.push(-maxBid * f); }
    return (
      <div className="vc-depth" onClick={(e) => e.stopPropagation()}>
        <div className="vc-depth-title">Глубина стакана (binance-style): <span style={{ color: '#e6725a' }}>аски ↑</span> / <span style={{ color: '#5fd08a' }}>биды ↓</span> · X — цена, Y — накопленный объём</div>
        <svg className="vc-chart" viewBox={`0 0 ${W} ${H}`} width="100%" preserveAspectRatio="xMidYMid meet">
          <rect x={ml} y={mt} width={pw} height={ph} fill="#0b1219" stroke="#1c2733" />
          {/* горизонтальные линии объёма + подписи оси Y */}
          {yticks.map((v, i) => (
            <g key={'y' + i}>
              <line x1={ml} y1={Y(v)} x2={ml + pw} y2={Y(v)} stroke="#182230" strokeDasharray="3 3" />
              <text x={ml - 6} y={Y(v) + 3} textAnchor="end" className="vc-chart-lbl">{fmtSize(Math.abs(v))}</text>
            </g>
          ))}
          {/* нулевая линия объёма (стык бид/аск у спреда) */}
          <line x1={ml} y1={Y(0)} x2={ml + pw} y2={Y(0)} stroke="#3a4a5a" />
          {/* вертикальные линии цены + подписи оси X */}
          {xticks.map((p, i) => (
            <g key={'x' + i}>
              <line x1={X(p)} y1={mt} x2={X(p)} y2={mt + ph} stroke="#121a23" />
              <text x={X(p)} y={mt + ph + 14} textAnchor="middle" className="vc-chart-lbl">{fmtPrice(p)}</text>
            </g>
          ))}
          {/* мид */}
          <line x1={X(mid)} y1={mt} x2={X(mid)} y2={mt + ph} stroke="#5a6a7a" strokeDasharray="2 3" />
          {/* площади + линии */}
          <polyline points={aFill} fill="rgba(230,90,90,0.18)" stroke="none" />
          <polyline points={bFill} fill="rgba(95,208,138,0.18)" stroke="none" />
          <polyline points={aLine} fill="none" stroke="#e6725a" strokeWidth="1.6" />
          <polyline points={bLine} fill="none" stroke="#5fd08a" strokeWidth="1.6" />
          <text x={ml} y={mt - 3} className="vc-chart-lbl">объём (base), аски ↑ / биды ↓</text>
          <text x={ml + pw} y={mt + ph + 14} textAnchor="end" className="vc-chart-lbl">цена ({symbol})</text>
        </svg>
      </div>
    );
  })();

  return (
    <div className="vc-chart-wrap">
      {ladder}
      {depthChart}
      <div className="vc-chart-ctrls" onClick={(e) => e.stopPropagation()}>
        <button type="button" className={paused ? 'vc-pause active' : 'vc-pause'}
          title={paused ? 'Возобновить живое обновление' : 'Остановить: заморозить график (без мигания); можно увеличивать и включать доп. элементы'}
          onClick={() => setPaused((p) => !p)}>{paused ? '▶ Старт' : '⏸ Стоп'}</button>
        {paused && <span className="vc-pause-badge" title="график заморожен">● пауза</span>}
        <span className="vc-zoom" title="Масштаб графика (увеличение во время паузы)">зум
          <button type="button" onClick={() => setZoom((z) => Math.max(1, Math.round((z - 0.5) * 10) / 10))} disabled={zc <= 1}>−</button>
          <span className="vc-zoom-val">{zc.toFixed(1)}×</span>
          <button type="button" onClick={() => setZoom((z) => Math.min(3.5, Math.round((z + 0.5) * 10) / 10))} disabled={zc >= 3.5}>+</button>
          {zc !== 1 && <button type="button" onClick={() => setZoom(1)} title="сброс масштаба">⤢</button>}
        </span>
        <label className="vc-cc-field">уровней покупки (ask)
          <input type="number" min="1" max={data.nAskMax || 1} value={nBuy}
            placeholder={String(data.nAskMax || '')} onChange={(e) => setNBuy(e.target.value)} />
        </label>
        <label className="vc-cc-field">уровней продажи (bid)
          <input type="number" min="1" max={data.nBidMax || 1} value={nSell}
            placeholder={String(data.nBidMax || '')} onChange={(e) => setNSell(e.target.value)} />
        </label>
        <span className="vc-orient">ориентация:
          <button className={vp ? 'active' : ''} onClick={() => setOrient('vp')}>объём–цена</button>
          <button className={!vp ? 'active' : ''} onClick={() => setOrient('pv')}>цена–объём</button>
        </span>
        <span className="vc-orient">anchor:
          <button className={anchorMode === 'mid' ? 'active' : ''} onClick={() => setAnchorMode('mid')}>mid</button>
          <button className={anchorMode === 'micro' ? 'active' : ''} onClick={() => setAnchorMode('micro')}>micro</button>
        </span>
        <label className="vc-cc-field">θ (safe share)
          <input type="number" min="0.01" max="1" step="0.05" value={theta}
            onChange={(e) => setTheta(e.target.value)} />
        </label>
        <label className="vc-cc-field vc-cc-raw"><input type="checkbox" checked={showRaw} onChange={(e) => setShowRaw(e.target.checked)} /> raw</label>
        <label className="vc-cc-field vc-cc-raw"><input type="checkbox" checked={showVwap} onChange={(e) => setShowVwap(e.target.checked)} /> VWAP</label>
        <label className="vc-cc-field vc-cc-raw"><input type="checkbox" checked={showSafe} onChange={(e) => setShowSafe(e.target.checked)} /> safe</label>
        <label className="vc-cc-field vc-cc-raw"><input type="checkbox" checked={showCe} onChange={(e) => setShowCe(e.target.checked)} /> CE зона</label>
        <label className="vc-cc-field vc-cc-raw"><input type="checkbox" checked={showFob}
          disabled={!(data.fobBid || []).length && !(data.fobAsk || []).length}
          onChange={(e) => setShowFob(e.target.checked)} /> FOB</label>
        <label className="vc-cc-field vc-cc-raw" title="Агрегатная линейная кривая ликвидности всех площадок этой пары"><input type="checkbox" checked={showAggAll}
          disabled={!(data.aggAll || []).length}
          onChange={(e) => setShowAggAll(e.target.checked)} /> Σ все площадки</label>
        <label className="vc-cc-field vc-cc-raw" title="Все площадки КРОМЕ отображаемой (этой venue)"><input type="checkbox" checked={showAggEx}
          disabled={!(data.aggEx || []).length}
          onChange={(e) => setShowAggEx(e.target.checked)} /> Σ без этой</label>
        <label className="vc-cc-field vc-cc-raw" title="§6.2 «Кривые котирования»: 4 способа снять наклон одного стакана (касательная / МНК / выгода / минорант). Отображаются как СЫРЫЕ линии (θ=1), сравнимы с VWAP. Выбор способа для клиринга — глобально во вкладке Clearing (панель конфига)."><input type="checkbox" checked={showMethods}
          disabled={!hasMethods}
          onChange={(e) => setShowMethods(e.target.checked)} /> 4 наклона §6.2</label>
        <label className="vc-cc-field vc-cc-raw" title="Индивидуальные линейные кривые ликвидности ВСЕХ площадок по этой паре валют (по кривой на биржу), в одном ценовом окне."><input type="checkbox" checked={showPerVenue}
          disabled={!hasPerVenue}
          onChange={(e) => setShowPerVenue(e.target.checked)} /> кривые всех площадок</label>
        <span className="vc-slope-badge" title="наклон клиринга (способ §6.2 выбран глобально в панели конфига Clearing)">клиринг M{selMethod}: β_T = <b>{fmtSig(data.betaT)}</b> quote/base · α_T = {fmtSig(data.alphaT)} · α_ext = {fmtSig(data.alphaExt)} (bind {data.bindSide || '—'} L{data.bindLevel || '—'}){loading ? ' …' : ''}</span>
      </div>
      <svg width={W} height={H} className="vc-chart" style={{ maxWidth: zc > 1 ? 'none' : '100%' }}>
        <line x1={ml} y1={mt} x2={ml} y2={mt + ph} stroke="#2a3a49" />
        <line x1={ml} y1={mt + ph} x2={ml + pw} y2={mt + ph} stroke="#2a3a49" />
        {volGuide(0, '#3a4a5a', '2 4', 'z')}
        {/* CE зона комиссии/бездействия: полоса |цена−anchor|≤c, где поток f=0 */}
        {showCe && Number.isFinite(deadLow) && Number.isFinite(deadHigh) && (vp
          ? <rect x={ml} y={Y(deadHigh)} width={pw} height={Math.abs(Y(deadLow) - Y(deadHigh))} fill="#e8c14a" opacity="0.12" />
          : <rect x={X(deadLow)} y={mt} width={Math.abs(X(deadHigh) - X(deadLow))} height={ph} fill="#e8c14a" opacity="0.12" />)}
        {Number.isFinite(anchor) && priceGuide(anchor, '#3a6a8a', '3 3', 'anc')}
        {bestBid > 0 && priceGuide(bestBid, '#3f6b52', '1 4', 'bb')}
        {bestAsk > 0 && priceGuide(bestAsk, '#7a5a3a', '1 4', 'ba')}
        {fobBid.length > 0 && <polyline points={line(fobBid)} fill="none" stroke="#4fb0d8" strokeWidth="1.1" strokeDasharray="5 3" />}
        {fobAsk.length > 0 && <polyline points={line(fobAsk)} fill="none" stroke="#d88fb0" strokeWidth="1.1" strokeDasharray="5 3" />}
        {safe.length > 0 && <polyline points={line(safe)} fill="none" stroke="#c9a0ff" strokeWidth="2.4" />}
        {/* §6.2: 4 кривые наклона (СЫРЫЕ, θ=1) — для сравнения с VWAP */}
        {mTangent.length > 0 && <polyline points={line(mTangent)} fill="none" stroke="#ff9f40" strokeWidth="1.3" strokeDasharray="3 2" />}
        {mLsq.length > 0 && <polyline points={line(mLsq)} fill="none" stroke="#40d0ff" strokeWidth="1.3" strokeDasharray="3 2" />}
        {mProfit.length > 0 && <polyline points={line(mProfit)} fill="none" stroke="#b980ff" strokeWidth="1.3" strokeDasharray="3 2" />}
        {mMinorant.length > 0 && <polyline points={line(mMinorant)} fill="none" stroke="#70d080" strokeWidth="1.3" strokeDasharray="3 2" />}
        {ceSell.length > 0 && <polyline points={line(ceSell)} fill="none" stroke="#e8c14a" strokeWidth="2.4" />}
        {ceBuy.length > 0 && <polyline points={line(ceBuy)} fill="none" stroke="#e8c14a" strokeWidth="2.4" />}
        {aggAll.length > 0 && <polyline points={line(aggAll)} fill="none" stroke="#7fe0e0" strokeWidth="2.2" strokeDasharray="7 3" />}
        {aggEx.length > 0 && <polyline points={line(aggEx)} fill="none" stroke="#e08fe0" strokeWidth="2" strokeDasharray="2 3" />}
        {/* Индивидуальная кривая ликвидности КАЖДОЙ площадки по этой паре */}
        {perVenue.map((v, i) => (v.curve || []).length > 0 &&
          <polyline key={'pv' + v.venue} points={line(v.curve)} fill="none"
            stroke={venueColor(v.venue, i)} strokeWidth="1.6" strokeDasharray="6 2" opacity="0.9" />)}
        {vwapBid.length > 0 && <polyline points={line(vwapBid)} fill="none" stroke="#2f8f66" strokeWidth="2" />}
        {vwapAsk.length > 0 && <polyline points={line(vwapAsk)} fill="none" stroke="#c07a2f" strokeWidth="2" />}
        {rawBid.length > 0 && <polyline points={line(rawBid)} fill="none" stroke="#5fd08a" strokeWidth="1.2" strokeDasharray="4 3" />}
        {rawBid.map((p, i) => <circle key={'br' + i} cx={X(xv(p))} cy={Y(yv(p))} r="1.9" fill="#5fd08a" />)}
        {rawAsk.length > 0 && <polyline points={line(rawAsk)} fill="none" stroke="#e6a15a" strokeWidth="1.2" strokeDasharray="4 3" />}
        {rawAsk.map((p, i) => <circle key={'ar' + i} cx={X(xv(p))} cy={Y(yv(p))} r="1.9" fill="#e6a15a" />)}
        <text x={ml} y={mt + ph + 16} textAnchor="middle" className="vc-chart-lbl">{fmt(xMin)}</text>
        <text x={ml + pw} y={mt + ph + 16} textAnchor="middle" className="vc-chart-lbl">{fmt(xMax)}</text>
        <text x={ml + pw / 2} y={mt + ph + 34} textAnchor="middle" className="vc-chart-lbl">{xTitle}</text>
        <text x={ml - 6} y={mt + 10} textAnchor="end" className="vc-chart-lbl">{fmt(yMax)}</text>
        <text x={ml - 6} y={mt + ph} textAnchor="end" className="vc-chart-lbl">{fmt(yMin)}</text>
        <text x={12} y={mt + ph / 2} textAnchor="middle" className="vc-chart-lbl" transform={`rotate(-90 12 ${mt + ph / 2})`}>{yTitle}</text>
      </svg>
      <div className="vc-chart-legend">
        {rawBid.length > 0 && <span className="vc-lg vc-lg-sell">▪ raw book продажа (bid)</span>}
        {rawAsk.length > 0 && <span className="vc-lg vc-lg-buy">▪ raw book покупка (ask)</span>}
        {vwapBid.length > 0 && <span className="vc-lg vc-lg-vwapsell">— VWAP продажа</span>}
        {vwapAsk.length > 0 && <span className="vc-lg vc-lg-vwapbuy">— VWAP покупка</span>}
        {safe.length > 0 && <span className="vc-lg vc-lg-safe">— safe translator P(q)=anchor+β_T·q</span>}
        {(ceSell.length > 0 || ceBuy.length > 0) && <span className="vc-lg" style={{ color: '#e8c14a' }}>— CE ликвидность (2 стороны, разрыв = спред+комиссия c={fmtSig(deadZonePm)}‰, биржа не торгует)</span>}
        {(fobBid.length > 0 || fobAsk.length > 0) && <span className="vc-lg vc-lg-fob">- - FOB-кривая venue</span>}
        {aggAll.length > 0 && <span className="vc-lg" style={{ color: '#7fe0e0' }}>— Σ ликвидность всех площадок пары ({(data.aggVenues || []).length})</span>}
        {aggEx.length > 0 && <span className="vc-lg" style={{ color: '#e08fe0' }}>·· Σ все кроме {data.aggExVenue || 'этой'}</span>}
        {perVenue.map((v, i) => (v.curve || []).length > 0 &&
          <span key={'pvlg' + v.venue} className="vc-lg" style={{ color: venueColor(v.venue, i) }}>- - {v.venue}</span>)}
        {showMethods && mTangent.length > 0 && <span className="vc-lg" style={{ color: '#ff9f40' }}>·· M1 касательная</span>}
        {showMethods && mLsq.length > 0 && <span className="vc-lg" style={{ color: '#40d0ff' }}>·· M2 МНК (трекает VWAP)</span>}
        {showMethods && mProfit.length > 0 && <span className="vc-lg" style={{ color: '#b980ff' }}>·· M3 по выгоде</span>}
        {showMethods && mMinorant.length > 0 && <span className="vc-lg" style={{ color: '#70d080' }}>·· M4 минорант (§6.3-safe)</span>}
        <span className="vc-lg vc-lg-anchor">- - anchor ({anchorMode}) {fmt(anchor)} · спред {Number(data.spreadBps).toFixed(2)} bps</span>
        {eng && <span className="vc-lg vc-lg-safe" title="то, что реально клирится (движок market_data)">клиринг: способ M{selMethod} · β_T={fmtSig(eng.betaT)} α_T={fmtSig(eng.alphaT)} [{eng.model}]</span>}
        {selMethod !== 4 && <span className="vc-lg" style={{ color: '#e0a04a' }} title="§6.3 «правило транслируемой глубины»: только минорант (M4) гарантированно не обещает больше, чем есть в книге. M1–M3 могут локально переобещать глубину.">⚠ клиринг-наклон ≠ минорант: возможно локальное переобещание глубины (§6.3)</span>}
      </div>
    </div>
  );
}

// Деталь одной строки клиринга: (1) исходные заявки, (2) клиринговые цены,
// ADR-060 — человекочитаемая причина + цвет строки диагностики sim-fill.
const REASON_LABEL = {
  no_limit_price: 'нет лимит-цены → симулятор пропускает матчинг',
  no_target_qty: 'нет target_qty',
  no_trade_feed: 'нет ленты публичных сделок venue',
  no_trades_in_window: 'нет публичных сделок в окне',
  none_cross_limit: 'ни одна публичная сделка не пересекла лимит',
  partial_window_volume: 'частично: объёма окна < target',
  filled: 'исполнено',
  rejected: 'отклонено venue',
};
const reasonLabel = (r) => REASON_LABEL[r] || r || '—';
const reasonBg = (r) => {
  if (r === 'filled') return '#e7f6e7';                 // зелёный — исполнено
  if (r === 'partial_window_volume') return '#fdf3d8';  // янтарь — частично
  return '#fbe4e4';                                     // красный — не исполнено
};

// Две таблицы позиций агентов (переводчики base/quote + арбитражёры источник/приёмник).
// Переиспользуется: статичный снимок батча (ClearingDetail) и живая панель (кнопка).
// Money-path/маршрут/ноги считает BFF — здесь только рендер (frontend-no-domain-compute).
export function AgentPositionsTables({ agentRows, title, onSelectAgent, selectedId, pnlTotals }) {
  const rows = Array.isArray(agentRows) ? agentRows : [];
  const clickable = typeof onSelectAgent === 'function';
  // Не действующие агенты (нет позиции и не двигались) показываются приглушённо, но на своём месте; агент, пропавший из
  // последнего ответа, остаётся со своей последней известной позицией (stale).
  const dimOf = (a) => (a.stale ? 0.4 : (a.inactive || isIdleAgent(a)) ? 0.6 : undefined);
  const rowProps = (a) => ({
    ...(clickable ? { onClick: () => onSelectAgent(a.agent_id) } : {}),
    style: {
      ...(clickable ? { cursor: 'pointer', background: a.agent_id === selectedId ? 'rgba(37,99,235,0.12)' : undefined } : {}),
      opacity: dimOf(a),
    },
    title: a.stale ? 'нет в последнем ответе — показана последняя известная позиция'
      : a.inactive ? 'агент в графе клиринга, но позиции у ledger ещё не было'
      : (clickable ? 'клик — график заработка агента во времени и разбор (цена/порог/хедж/исполнение)' : undefined),
  });
  const sgn = (n) => (Number(n) > 1e-9 ? '+' : '') + fmtNum(n);
  const dash = <span title="нет данных" style={{ opacity: 0.4 }}>—</span>;
  // Ячейка знакового числа (было/дельта/стало) в валюте ноги; null ⇒ прочерк.
  const numCell = (v) => (v == null) ? dash : (
    <span className={Number(v) > 1e-9 ? 'vc-side-ask' : Number(v) < -1e-9 ? 'vc-side-bid' : ''}>{sgn(v)}</span>
  );
  const deltaCell = (v, hasDelta) => (!hasDelta || v == null)
    ? <span title="агент не двигался в этом такте" style={{ opacity: 0.35 }}>0</span>
    : numCell(v);
  // Заявка агента на внешнюю биржу: сколько отправлено (target_qty) и сколько исполнилось (накопленно, с дозаполнением
  // резидентных лимитов). Показывается в строке той валюты, которую заявка торгует (BTC/USDT → в строке BTC), в единицах этой
  // валюты. h: объект заявки | null (заявки нет — прочерк) | undefined (в этой строке заявки быть не может — пусто).
  // Цвет исполнения: зелёный=полностью, янтарь=частично, красный=0.
  const unitTag = (cur) => <span style={{ opacity: 0.6, fontSize: '0.85em' }}> {cur}</span>;
  const hedgeSent = (h, cur) => {
    if (h === undefined) return null;
    if (h === null) return dash;
    return <span title={`заявка: ${h.symbol || ''} ${h.side || ''}, статус ${h.status}`}>{fmtNum(h.sent)}{unitTag(cur)}</span>;
  };
  const hedgeFilled = (h, cur) => {
    if (h === undefined) return null;
    if (h === null) return dash;
    const s = Number(h.sent) || 0, f = Number(h.filled) || 0;
    const col = f <= 1e-12 ? '#e06060' : (f + 1e-9 >= s ? '#5fd08a' : '#e0b84a');
    const lbl = f <= 1e-12 ? 'не исполнено' : (f + 1e-9 >= s ? 'полностью' : 'частично');
    return <span style={{ color: col }} title={`${lbl} (${h.status})`}>{fmtNum(f)}{unitTag(cur)}</span>;
  };
  // Статистика по СОСТОЯВШИМСЯ сделкам (без комиссий): кол-во валюты (signed) и заработок USDT.
  const tradeQty = (v, cur) => (v == null || !Number.isFinite(Number(v))) ? dash : (
    <span className={Number(v) > 1e-12 ? 'vc-side-ask' : Number(v) < -1e-12 ? 'vc-side-bid' : ''}>
      {sgn(v)}<span style={{ opacity: 0.6, fontSize: '0.85em' }}> {cur}</span>
    </span>
  );
  // ADR-072 §C: realized (R — закрытые round-trip, НАРАСТАЕТ = заработок) основным,
  // unrealized (U — MTM открытого остатка, болтается с маркой) приглушённым: «R / U».
  const pnlColor = (x) => Number(x) > 1e-9 ? '#5fd08a' : Number(x) < -1e-9 ? '#e06060' : '#9fb2c2';
  // Заработок агента на внешних биржах (R / U): числа пришли готовыми из ledger (ADR-087 п. 10) — тот же расчёт и то же хранилище, что у
  // вкладки «PnL». Здесь только отображение. Нет данных (нет цены оценки) — прочерк.
  const tradeCell = (a) => {
    const r = a.realized_usdt, u = a.unrealized_usdt;
    const rOk = r != null && Number.isFinite(Number(r));
    const uOk = u != null && Number.isFinite(Number(u));
    if (!rOk) return null;
    return (
      <span title="Внешние биржи, USDT, без комиссий; считает ledger. R — реализованный (зафиксирован сделками), U — нереализованный (остаток, которому не нашлось встречной позиции на внутренней бирже, по текущей цене оценки). Внутренняя биржа и итого — на вкладке «PnL». Клик по агенту — график во времени">
        <span style={{ color: pnlColor(r), fontWeight: 600 }}>{sgn(r)}</span>
        {uOk && <span style={{ color: pnlColor(u), opacity: 0.6, fontSize: '0.85em' }}> / {sgn(u)}</span>}
      </span>
    );
  };
  const okNum = (x) => x != null && Number.isFinite(Number(x));
  const pnlCell = (a) => {
    const trade = tradeCell(a);
    if (a.agent_kind !== 'arbitrageur') return trade || dash;
    // Арбитражёр: под заработком по сделкам — две величины его позиции по ценам бирж (НЕ результат): «базис» — разница цен
    // между биржами на перевозимом объёме; «открытая позиция» — нетто-количество актива на двух ногах (≠ 0, когда ноги
    // исполняются несимметрично). Открытая позиция — риск изменения цены, а не убыток.
    const hasPos = okNum(a.basis_mtm_usdt) || okNum(a.open_position_usdt);
    if (!trade && !hasPos) return dash;
    return (
      <span data-testid="arb-pnl">
        {trade || <span title="внешних сделок пока нет" style={{ opacity: 0.4 }}>—</span>}
        {hasPos && (
          <span data-testid="arb-basis" data-ui-id="UI-VC-POS-BASIS" style={{ display: 'block', fontSize: '0.8em', opacity: 0.85 }}
            title="Позиция арбитражёра по ценам его бирж, USDT. Базис = разница цен между биржами на перевозимом объёме (результат разницы цен, не реализован). Открытая позиция = нетто-количество актива на двух ногах × цена: у зеркальной позиции 0, растёт, когда хедж одной ноги исполняется, а другой нет; это риск изменения цены, а не убыток.">
            базис {okNum(a.basis_mtm_usdt) ? <span style={{ color: pnlColor(a.basis_mtm_usdt) }}>{sgn(a.basis_mtm_usdt)}</span> : '—'}
            {' · '}открытая позиция {okNum(a.open_position_usdt) ? <span style={{ color: pnlColor(a.open_position_usdt) }}>{sgn(a.open_position_usdt)}</span> : '—'}
          </span>
        )}
      </span>
    );
  };
  // Выгода участника в клиринге (CALC-CE-CLEARING-SURPLUS): накоплена BFF по тактам с момента сброса статистики. Это оценка относительно
  // кривой агента, а не деньги в кассе; не складывается с R и U.
  const usd2 = (v) => Number(v).toLocaleString('ru-RU', { minimumFractionDigits: 2, maximumFractionDigits: 2 });
  const surplusCell = (a) => {
    if (a.surplus_usdt == null || !Number.isFinite(Number(a.surplus_usdt))) return dash;
    const cov = a.surplus_coverage != null && Number.isFinite(Number(a.surplus_coverage)) ? Math.round(100 * Number(a.surplus_coverage)) : null;
    return (
      <span style={{ color: '#b58cff' }} title={`Выгода участника в клиринге по теории (w²/2α по тактам), накоплено с сброса статистики, USDT. Оценка относительно кривой агента, не деньги в кассе.${cov != null ? ' Учтено ' + cov + ' % тактов.' : ''}`}>
        {usd2(a.surplus_usdt)}
      </span>
    );
  };
  // Две ноги агента (по валюте): переводчик base/quote; арбитражёр asset@src / asset@dst. К каждой ноге BFF-данные внешней стороны:
  //   hedge — заявка этой ноги (объект | null «заявки нет» | undefined «в этой строке заявки не бывает»);
  //   trades — состоявшиеся сделки в единицах валют строки: [{ v, cur }] (переводчик — одна величина, арбитражёр — база и квота биржи ноги).
  const legsOf = (a) => {
    const quoteCur = a.quote || 'USDT';
    const top = (a.hedge_status == null) ? null : {
      symbol: a.hedge_symbol, side: a.hedge_side, sent: a.hedge_sent, filled: a.hedge_filled, status: a.hedge_status,
    };
    if (a.agent_kind === 'arbitrageur') {
      const r = a.arb || {};
      const hv = a.hedge_by_venue;          // последняя заявка на каждой из бирж агента
      const tb = a.trade_by_venue;          // сделки по биржам: { venue: { traded_base, traded_quote } }
      const hedgeAt = (venue) => {
        if (hv) return hv[venue] || null;
        return top && a.hedge_venue === venue ? top : null;   // снимок без разбивки по биржам: верхняя заявка → нога её биржи
      };
      const tradesAt = (venue, first) => {
        if (tb) {
          const t = tb[venue];
          return t ? [{ v: t.traded_base, cur: a.base }, { v: t.traded_quote, cur: quoteCur }] : null;
        }
        // нет разбивки по биржам — суммарные сделки целиком в первой строке
        return first ? [{ v: a.traded_base, cur: a.base }, { v: a.traded_quote, cur: quoteCur }] : null;
      };
      return [
        { label: `${a.base}@${r.src || '?'}`, cur: a.base, before: r.bleg_src, delta: r.dleg_src, after: r.leg_src, hedge: hedgeAt(r.src), trades: tradesAt(r.src, true) },
        { label: `${a.base}@${r.dst || '?'}`, cur: a.base, before: r.bleg_dst, delta: r.dleg_dst, after: r.leg_dst, hedge: hedgeAt(r.dst), trades: tradesAt(r.dst, false) },
      ];
    }
    // Переводчик: заявка торгует символ BASE/QUOTE — показываем её в строке базовой валюты символа (BTC/USDT → строка BTC).
    const labels = [a.base, quoteCur];
    const hs = String(a.hedge_symbol || '');
    // символ приходит как BASE/QUOTE; без разделителя (BTCUSDT) базой считается та валюта строки, с которой символ начинается
    const hedgedCur = hs.includes('/') ? hs.split('/')[0] : (labels.find((l) => hs.startsWith(l)) || a.base);
    const hedgedIdx = Math.max(0, labels.indexOf(hedgedCur));
    const hedgeOf = (idx) => (idx === hedgedIdx ? top : (top ? undefined : (idx === 0 ? null : undefined)));
    return [
      { label: a.base, cur: a.base, before: a.pos_base_before, delta: a.delta_base, after: a.pos_base, hedge: hedgeOf(0), trades: [{ v: a.traded_base, cur: a.base }] },
      { label: quoteCur, cur: quoteCur, before: a.pos_quote_before, delta: a.delta_quote, after: a.pos_quote, hedge: hedgeOf(1), trades: [{ v: a.traded_quote, cur: quoteCur }] },
    ];
  };
  // Состоявшиеся сделки одной строки: база и квота (арбитражёр — обе на бирже ноги, друг под другом), в одном столбце.
  const tradesCell = (lg) => (
    !lg.trades ? dash : lg.trades.map((t, k) => <div key={k}>{tradeQty(t.v, t.cur)}</div>)
  );
  // Агент = ДВЕ строки (по ноге/валюте). Слева — внутренняя биржа CE (было → дельта → стало по каждой ноге), справа — внешняя
  // (заявка и сделки — в строке своей валюты; заработок общий по агенту, rowSpan=2).
  const renderAgent = (a, i, tag) => {
    const L = legsOf(a);
    return L.map((lg, li) => (
      <tr key={`${a.agent_id || ''}|${tag}|${li}`} {...rowProps(a)}>
        {li === 0 && <td className="vc-mono" rowSpan={2}>{a.agent_id}{a.venue ? <span style={{ opacity: 0.5 }}> · {a.venue}</span> : ''}</td>}
        <td className="vc-mono">{lg.label}</td>
        <td className="vc-mono">{numCell(lg.before)}</td>
        <td className="vc-mono">{deltaCell(lg.delta, a.hasDelta)}</td>
        <td className="vc-mono"><b>{numCell(lg.after)}</b></td>
        {li === 0 && <td className="vc-mono" rowSpan={2} data-testid="surplus-cell">{surplusCell(a)}</td>}
        <td className="vc-mono vc-ext-start" data-testid="hedge-sent">{hedgeSent(lg.hedge, lg.cur)}</td>
        <td className="vc-mono" data-testid="hedge-filled">{hedgeFilled(lg.hedge, lg.cur)}</td>
        <td className="vc-mono" data-testid="leg-trades">{tradesCell(lg)}</td>
        {li === 0 && <td className="vc-mono" rowSpan={2}>{pnlCell(a)}</td>}
      </tr>
    ));
  };
  const tRows = rows.filter((a) => a.agent_kind === 'translator');
  const aRows = rows.filter((a) => a.agent_kind === 'arbitrageur');
  // Группы по биржам и фиксированный порядок — не зависят от хода клиринга (двигался агент или нет).
  const tGroups = groupTranslators(rows);
  const aGroups = groupArbitrageurs(rows);
  const groupHead = (text) => (
    <tr className="vc-group-head"><td colSpan={10}>{text}</td></tr>
  );
  // Итоги заработка (внешние биржи, USDT, без комиссий): переводчики, арбитражёры и вместе. Их считает ledger (GetAgentPnl: итоги по видам и
  // общий) — панель только показывает; складывать строки в браузере нельзя (ADR-087 п. 10). Базис и открытая позиция арбитражёров, как и
  // выгода по теории, в заработок не входят — они показаны отдельно (и пока суммируются здесь: не R/U, см. known-gaps).
  const sumKey = (arr, key) => arr.reduce((acc, a) => acc + (okNum(a[key]) ? Number(a[key]) : 0), 0);
  const extOf = (kind) => {
    const g = pnlTotals && pnlTotals[kind] && pnlTotals[kind].external;
    return g && g.has_data ? g : null;
  };
  const tPnl = extOf('translator'), aPnl = extOf('arbitrageur'), allPnl = extOf('all');
  // Подпись «(N)»: число агентов в итоге ledger и сколько из них без данных (нет цены оценки — в сумму они не входят); нет итога — число строк.
  const kindCount = (kind, rowsLen) => {
    const t = pnlTotals && pnlTotals[kind];
    if (!t || t.agent_count == null) return rowsLen;
    return t.incomplete_agents > 0 ? `${t.agent_count}, без данных: ${t.incomplete_agents}` : t.agent_count;
  };
  const haveStats = rows.length > 0;  // блок итогов есть всегда, когда есть агенты: нет итогов ledger — явное «нет данных», а не пустое место
  const arbBasis = sumKey(aRows, 'basis_mtm_usdt'), arbOpen = sumKey(aRows, 'open_position_usdt');
  const tSur = sumKey(tRows, 'surplus_usdt'), aSur = sumKey(aRows, 'surplus_usdt');
  const haveSurplus = rows.some((a) => okNum(a.surplus_usdt));
  const hasArbPos = aRows.some((a) => okNum(a.basis_mtm_usdt) || okNum(a.open_position_usdt));
  const pnlNum = (v) => (
    <b style={{ color: v > 1e-9 ? '#5fd08a' : v < -1e-9 ? '#e06060' : '#9fb2c2' }}>{sgn(v)}</b>
  );
  // Двухъярусная шапка: слева — внутренняя биржа CE (позиция агента в клиринге), справа — внешние биржи (хедж и результат).
  const head = (
    <thead>
      <tr>
        <th rowSpan={2}>agent_id</th>
        <th rowSpan={2} title="валюта/нога агента (отдельная строка на каждую)">нога</th>
        <th colSpan={4} className="vc-grp-int" data-testid="grp-internal"
          title="Внутренняя биржа CE: позиция агента в клиринге Continuous Exchange. Считается внутри CE, на внешние биржи не уходит.">
          Внутренняя биржа CE — позиция агента
        </th>
        <th colSpan={4} className="vc-grp-ext vc-ext-start" data-testid="grp-external"
          title="Внешняя биржа: заявки агента на реальные биржи (хедж, когда позиция вышла за полосу) и состоявшиеся сделки. Строка = валюта заявки.">
          Внешняя биржа — хедж и результат
        </th>
      </tr>
      <tr>
        <th title="позиция ДО этого такта клиринга">было</th>
        <th title="Δ позиции в ЭТОМ такте">дельта</th>
        <th title="позиция ПОСЛЕ такта (= было + дельта), считается от полной позиции c + отправленное в заявках (in_flight) — так же, как на вкладке «Граф»">стало</th>
        <th data-testid="surplus-head" title="Выгода участника в клиринге по теории: S = w²/(2α) за такт, накоплено с момента сброса статистики, USDT. Узкое звено цикла получает больше. Оценка относительно кривой агента, не деньги в кассе: деньгами становится при закрытии позиции (хедж у переводчика, физический перевод у связки).">
          выгода по теории
          <div style={{ fontWeight: 400, fontSize: '0.85em', opacity: 0.85 }}>S, USDT</div>
        </th>
        <th className="vc-ext-start" title="сколько отправлено на биржу (band-хедж заявка, target_qty). Показано в строке той валюты, которую торгует заявка: заявка по BTC/USDT — в строке BTC">отправлено</th>
        <th title="сколько исполнилось по результатам симуляции (накопленно, с дозаполнением резидентных лимитов). Та же строка, что у «отправлено»">исполнено</th>
        <th title="состоявшиеся сделки агента, количество валюты строки (знаковое: покупка +, продажа −); без комиссий. В строке базовой валюты — база, в строке котируемой — квота; у арбитражёра в каждой строке обе валюты его биржи">сделки</th>
        <th data-testid="ru-head"
          title="Заработок на внешних биржах, USDT, без комиссий; считает ledger. R — реализованный (уже зафиксирован сделками), U — нереализованный (остаток без встречной позиции на внутренней бирже, по текущей цене оценки). R + U — итог. Внутренняя биржа и итого по двум биржам — на вкладке «PnL». Подробно — в пояснении под таблицами.">
          заработок R / U ⓘ
          <div style={{ fontWeight: 400, fontSize: '0.85em', opacity: 0.85 }}>R реализ. · U нереализ.</div>
        </th>
      </tr>
    </thead>
  );
  return (
    <div className="vc-sec">
      <div className="vc-sec-title">{title}</div>
      <div className="vc-sec-body">
        <div className="vc-sub-head">Переводчики по биржам (пара base/quote) — <b>{tRows.length}</b></div>
        {tRows.length > 0 ? (
          <table className="vc-sub-table">{head}
            {tGroups.map((g) => (
              <tbody key={`t|${g.venue}`} data-testid={`t-group-${g.venue}`}>
                {groupHead(`${g.venue || 'без биржи'} — переводчики: ${g.rows.length}`)}
                {g.rows.map((a, i) => renderAgent(a, i, 't'))}
              </tbody>
            ))}
          </table>
        ) : <div className="vc-note vc-note-tight">нет переводчиков</div>}

        <div className="vc-sub-head" style={{ marginTop: 14 }}>Арбитражёры по парам бирж (перевоз актива между площадками) — <b>{aRows.length}</b></div>
        {aRows.length > 0 ? (
          <table className="vc-sub-table">{head}
            {aGroups.map((g) => (
              <tbody key={`a|${g.key}`} data-testid={`a-group-${g.key}`}>
                {groupHead(`${g.key} — арбитражёры: ${g.rows.length}`)}
                {g.rows.map((a, i) => renderAgent(a, i, 'a'))}
              </tbody>
            ))}
          </table>
        ) : <div className="vc-note vc-note-tight">нет арбитражёров</div>}
        <div className="vc-note vc-note-tight vc-ru-legend" data-testid="ru-explain">
          <b>Что такое R и U в колонке «заработок».</b> Оба числа — USDT, <b>без комиссий</b>, это <b>внешние биржи</b>, и считает их <b>ledger</b>
          (те же числа, что на вкладке «PnL»; там же внутренняя биржа CE и итого). Накапливаются с запуска учёта или последнего сброса позиций.
          <ul>
            <li>
              <b>R — реализованный заработок</b>: уже зафиксирован сделками, сам по себе не меняется, пока нет новых сделок.
              У переводчика это накопленная по каждой сделке разница между ценой исполнения и справедливой ценой клиринга: продажа дороже
              справедливой цены — плюс, покупка дешевле — плюс. У связки внешние сделки редки (рыночный хедж её ног выключен): её результат —
              на внутренней бирже, на вкладке «PnL».
            </li>
            <li>
              <b>U — нереализованный результат</b>: оценка по текущей цене только того внешнего объёма, которому не нашлось встречной позиции на
              внутренней бирже. Хедж закрывает внутреннюю позицию, поэтому у переводчика U здесь обычно близок к нулю, а открытый риск виден в
              внутренней группе на вкладке «PnL».
            </li>
            <li><b>R + U</b> — сумма по внешним биржам. График по клику на агента показывает оба ряда во времени.</li>
          </ul>
          Базис и открытая позиция арбитражёров (под R / U) — это позиция, а не заработок.
          <br /><b>Выгода по теории (S)</b> — отдельная колонка во внутренней группе: сколько участник выигрывает в клиринге каждый такт, w²/(2α), накоплено с сброса статистики. Это оценка относительно его кривой, а не деньги в кассе;
          у связки, которая больше не торгует на внешних биржах (ADR-083), это основной показатель работы.
        </div>
        {aRows.length > 0 && (
          <div className="vc-note vc-note-tight" data-testid="arb-explain">
            <b>Как читать заработок арбитражёров.</b> Арбитражёр перемещает актив между двумя биржами внутри CE и, как переводчик,
            <b> выходит на внешнюю биржу</b>, когда его позиция достигает порога (ADR-080). Его заработок — колонка R / U по состоявшимся
            сделкам; тариф перевоза внутри клиринга сейчас 0. Под ним показаны две величины <b>позиции</b>, а не результата:
            <b> базис</b> — разница цен между биржами на перевозимом объёме (сейчас доли USDT), и <b>открытая позиция</b> — нетто-количество актива
            на двух ногах по ценам бирж. У зеркальной позиции она 0; она растёт, когда хедж одной ноги исполняется, а другой нет (на Kraken и
            Coinbase заявки почти не исполняются), и тогда нога остаётся открытой: это <b>риск изменения цены, а не убыток</b>.
            Прибыль от разницы цен внутри клиринга по базовому документу уходит на счёт дома, а не агенту.
          </div>
        )}

        {haveStats && (
          <div className="vc-trade-totals" data-testid="trade-totals">
            <span className="vc-tt-title">Общий заработок на внешних биржах (из ledger, USDT, без комиссий):</span>
            {pnlTotals ? (
              <>
                <span>Переводчики ({kindCount('translator', tRows.length)}): R {tPnl ? pnlNum(tPnl.r) : dash} / U {tPnl ? pnlNum(tPnl.u) : dash}</span>
                <span>Арбитражёры ({kindCount('arbitrageur', aRows.length)}): R {aPnl ? pnlNum(aPnl.r) : dash} / U {aPnl ? pnlNum(aPnl.u) : dash}</span>
                <span className="vc-tt-all" data-testid="total-earnings" title="Сумма заработка на внешних биржах по всем агентам, посчитана в ledger: R — реализованный, U — нереализованный. Базис и открытая позиция арбитражёров сюда не входят. Внутренняя биржа и итого — на вкладке «PnL».">
                  Вместе: R {allPnl ? pnlNum(allPnl.r) : dash} / U {allPnl ? pnlNum(allPnl.u) : dash} · итого R+U {allPnl ? pnlNum(allPnl.r_plus_u) : dash}
                </span>
              </>
            ) : (
              <span data-testid="pnl-totals-missing" title="Итоги считает ledger; он не ответил или учёт выключен.">нет данных от ledger</span>
            )}
            {haveSurplus && (
              <span data-testid="surplus-totals" title="Выгода участников в клиринге по теории, накоплено с сброса статистики. Не деньги в кассе и не складывается с R и U.">
                Выгода в клиринге (теория): переводчики <b style={{ color: '#b58cff' }}>{usd2(tSur)}</b> · арбитражёры <b style={{ color: '#b58cff' }}>{usd2(aSur)}</b>
              </span>
            )}
            {hasArbPos && (
              <span data-testid="arb-pos-total" title="Не заработок. Базис — результат разницы цен между биржами на перевозимом объёме; открытая позиция — нетто-количество актива на ногах арбитражёров по ценам бирж (риск изменения цены, не убыток).">
                Позиции арбитражёров (не заработок): базис {pnlNum(arbBasis)} · открытая позиция {pnlNum(arbOpen)}
              </span>
            )}
          </div>
        )}

        <div className="vc-note vc-note-tight">
          Каждый агент — <b>две строки (по валюте/ноге)</b>. <b>Слева — внутренняя биржа CE</b>: <b>было → дельта → стало</b> этого такта.
          <b> Справа — внешние биржи</b>: заявка и сделки стоят в строке той валюты, которую торгует заявка (по BTC/USDT — в строке BTC).
          <b> Переводчик</b> — пара base/quote (одна нога +, другая −). <b>Арбитражёр</b> — актив на источнике (−) и приёмнике (+); «v3» в id — площадка <code>uniswap_v3</code>; его заявки и сделки показаны по бирже каждой ноги.
          <b> Отправлено/исполнено</b> — последняя band-хедж-заявка: сколько ушло на биржу и сколько исполнилось по симуляции (<span style={{ color: '#5fd08a' }}>полностью</span> / <span style={{ color: '#e0b84a' }}>частично</span> / <span style={{ color: '#e06060' }}>не исполнено</span>).
          <b> Сделки</b> — по ВСЕМ состоявшимся сделкам агента (CALC-F12-HEDGE-PNL, без комиссий): количество валюты строки (знаковое: покупка +, продажа −); база и квота теперь в одном столбце — в строке базовой валюты база, в строке котируемой квота. Ноги/заявки/сделки считает BFF (money-path, ADR-057).
        </div>
      </div>
    </div>
  );
}

// Drill-down по выбранному агенту: позиция → сдвиг цены от позиции (skew) → порог q
// → пробой → хедж-заявка в паре → публичные сделки → имитируемое (полное/частичное)
// исполнение. Данные из /agent-detail (BFF), здесь только рендер.
function AgentDrillDown({ detail }) {
  // Живые часы: возраст времён (запрос/ответ ленты, исполнение сделки, симуляция хеджа)
  // тикает раз в секунду, чтобы разбор времён обновлялся динамически, а не был снимком.
  const [nowMs, setNowMs] = useState(Date.now());
  useEffect(() => { const id = setInterval(() => setNowMs(Date.now()), 1000); return () => clearInterval(id); }, []);
  if (!detail) return <div className="vc-note vc-note-tight">кликните агента в таблице выше — покажу цену/порог/хедж/исполнение.</div>;
  const a = detail.agent || {};
  const sk = detail.skew || {};
  const b = detail.band || {};
  const vol = detail.volatility || null;  // F-18 D1+#7 (704): σ + превью динамического порога
  const h = detail.hedge;
  const isT = a.agent_kind === 'translator';
  const c = Number(a.c_position) || 0;               // неотправленный остаток (A7)
  const inFlight = Number(a.in_flight) || 0;          // отправлено в заявку (A7)
  const cTotal = (a.c_total != null) ? Number(a.c_total) : (c + inFlight);  // истинное обязательство
  const num = (v, dd = 6) => (v == null || v === '') ? '—' : Number(v).toFixed(dd);
  // c_j/q/избыток — в k-USDT (тысячи USDT). Показываем в K USDT (как задаётся порог
  // «18 K»): kusd(18) = «18 K USDT», kusd(292.5) = «292.5 K USDT». K = тысяч USDT.
  const kusd = (kv) => (kv == null) ? '—' : Number(kv).toLocaleString('ru-RU', { maximumFractionDigits: 1 }) + ' K USDT';
  const GREEN = '#5fd08a', RED = '#e6725a', MUTE = '#8a9aa8';
  const posColor = (v) => v > 1e-9 ? GREEN : v < -1e-9 ? RED : MUTE;
  const ps = Number(sk.priceShiftPm) || 0;
  const psColor = posColor(ps);
  const box = { border: '1px solid #1c2733', background: '#101821', borderRadius: 8, padding: '8px 12px', marginBottom: 8 };
  const head = { fontWeight: 600, marginBottom: 4, color: '#cdd9e4' };
  // Живой возраст: nowMs − ts_ms (тикает). fmtAgo — «N.N с назад» / «N мс назад».
  const agoMs = (tsMs) => (tsMs == null || tsMs <= 0) ? null : Math.max(0, nowMs - Number(tsMs));
  const fmtAgo = (ms) => ms == null ? '—' : (ms >= 1000 ? (ms / 1000).toFixed(1) + ' с назад' : Math.round(ms) + ' мс назад');
  const fmtMs = (ms) => ms == null ? '—' : (ms >= 1000 ? (ms / 1000).toFixed(1) + ' с' : Math.round(ms) + ' мс');
  return (
    <div style={{ padding: '4px 2px', color: '#dfe6ee' }}>
      <div style={box}>
        <b>{a.agent_id}</b> · {isT ? 'переводчик' : 'арбитражёр'} · пара <b>{a.pair}</b> · обязательство ={' '}
        <b style={{ color: posColor(cTotal) }}>{kusd(cTotal)}</b>
        <div style={{ fontSize: 12, color: MUTE, marginTop: 2 }}>
          = c_j (неотправл.) <b style={{ color: posColor(c) }}>{kusd(c)}</b> + in_flight (в заявке) <b>{kusd(inFlight)}</b>{' '}
          <span title="Модель A7: при пробое избыток уходит из c_j в заявку (c→±q); истинная накопленная позиция = c_j + in_flight, она и сходится к band">ⓘ</span>
        </div>
      </div>
      <div style={box}>
        <div style={head}>Сдвиг цены от позиции (inventory-skew)</div>
        <div style={{ fontSize: 13 }}>anchor_eff = anchor − clamp(γ·(c_j+in_flight)), γ={sk.gamma}, клэмп={sk.clamp}‰</div>
        <div>текущий сдвиг цены:{' '}
          <b style={{ color: psColor }}>{ps >= 0 ? '+' : ''}{ps.toFixed(3)} ‰</b>{' '}
          ({cTotal > 1e-9 ? 'обязательство + ⇒ цена ↓ (возврат к нулю)' : cTotal < -1e-9 ? 'обязательство − ⇒ цена ↑ (возврат к нулю)' : 'обязательство 0 ⇒ сдвига нет'})
        </div>
        {/* Таблица сдвига: якорь → дельта → сдвинутая цена; в ед. 2-й валюты пары и в USDT.
            Числа считает BFF (skewTable), здесь только форматирование. */}
        {(() => {
          const st = detail.skewTable;
          if (!st || !st.available) {
            return <div style={{ fontSize: 12, color: MUTE, marginTop: 6 }}>таблица сдвига недоступна: {st && st.reason ? st.reason : 'нет данных'}</div>;
          }
          // Число знаков по величине якоря — одинаково для всех строк столбца.
          const dd = (v) => { const m = Math.abs(v); return m >= 1000 ? 4 : m >= 1 ? 6 : 10; };
          const fx = (v, ref, signed) => {
            const t = Number(v).toLocaleString('ru-RU', { minimumFractionDigits: dd(ref), maximumFractionDigits: dd(ref) });
            return signed && v > 0 ? '+' + t : t;
          };
          const dcol = (v) => (v > 0 ? GREEN : v < 0 ? RED : MUTE);
          const baseCcy = st.base || 'база';
          const legsTag = st.position.legs ? ' (Σ ног)' : '';
          const rows = [
            { n: '1) Цена якоря', q: st.anchor.quote, u: st.anchor.usdt, sg: false },
            { n: '2) Дельта сдвига', q: st.delta.quote, u: st.delta.usdt, sg: true },
            { n: '3) Сдвинутая цена', q: st.shifted.quote, u: st.shifted.usdt, sg: false },
            // Позиция (c_j + in_flight) — в обеих валютах пары: по 1-й (базовой) и по 2-й (котируемой).
            // Знаки противоположны: держишь базовую (+) ⇔ должен котируемую (−). USDT-стоимость
            // базовой ноги = −usdt (та же величина с обратным знаком). Столбец «ед. валюты»
            // для этих строк самоподписан своей валютой (posCcy).
            { n: `4) Позиция по 1-й валюте (${baseCcy})${legsTag}`, q: st.position.base, u: (st.position.usdt == null ? null : -st.position.usdt), sg: true, own: true, posCcy: baseCcy },
            { n: `5) Позиция по 2-й валюте (${st.quote})${legsTag}`, q: st.position.quote, u: st.position.usdt, sg: true, own: true, posCcy: st.quote },
            // Пороги позиции (по модулю |позиция|):
            { n: '6) Порог входа в хедж Z̄lim (мейкер)', q: st.thresholds.zLim.quote, u: st.thresholds.zLim.usdt, sg: false, own: true },
            { n: '7) Порог тейкер-сброса Z̄mkt', q: st.thresholds.zMkt.quote, u: st.thresholds.zMkt.usdt, sg: false, own: true },
            { n: '8) Пол полосы q_floor', q: st.thresholds.floor.quote, u: st.thresholds.floor.usdt, sg: false, own: true },
          ];
          return (
            <table className="vc-sub-table" style={{ marginTop: 6 }}>
              <thead><tr>
                <th>Показатель</th>
                <th style={{ textAlign: 'right' }}>В ед. 2-й валюты ({st.quote})</th>
                <th style={{ textAlign: 'right' }}>В USDT</th>
              </tr></thead>
              <tbody>
                {rows.map((r, i) => (
                  <tr key={i}>
                    <td>{r.n}</td>
                    <td className="vc-mono" style={{ textAlign: 'right', color: r.sg ? dcol(r.q) : undefined }}>{r.q == null ? '—' : (fx(r.q, r.own ? Math.max(Math.abs(r.q), 1) : st.anchor.quote, r.sg) + (r.posCcy ? ' ' + r.posCcy : ''))}</td>
                    <td className="vc-mono" style={{ textAlign: 'right', color: r.sg ? dcol(r.u) : undefined }}>{r.u == null ? '—' : fx(r.u, r.own ? Math.max(Math.abs(r.u), 1) : st.anchor.usdt, r.sg)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          );
        })()}
        {detail.skewTable && detail.skewTable.position && detail.skewTable.position.legs && (
          <div style={{ fontSize: 11, color: MUTE, marginTop: 4 }}>
            ноги арбитражёра: {detail.skewTable.position.legs.map((l, i) => (
              <span key={i} style={{ marginRight: 10 }}>
                {l.venue || '—'} <b style={{ color: posColor(l.usdt) }}>{Number(l.usdt).toLocaleString('ru-RU', { maximumFractionDigits: 2 })} USDT</b>
                {' '}({Number(l.quote).toLocaleString('ru-RU', { maximumFractionDigits: 6 })} {detail.skewTable.quote})
              </span>
            ))}
          </div>
        )}
        {detail.skewTable && detail.skewTable.note && <div style={{ fontSize: 11, color: '#e0b84a', marginTop: 4 }}>{detail.skewTable.note}</div>}
        <div style={{ fontSize: 11, color: MUTE, marginTop: 4 }}>
          пороги 5–7 — по модулю позиции: |позиция| &gt; Z̄lim ⇒ хедж (мейкер), &gt; Z̄mkt ⇒ тейкер; q_floor — нижняя граница Z̄lim ·{' '}
          сдвинутая = якорь·exp(сдвиг‰/1000) · курс 2-й валюты: 1 {(detail.skewTable && detail.skewTable.quote) || ''} ={' '}
          {detail.skewTable && detail.skewTable.available ? Number(detail.skewTable.quoteUsdt).toLocaleString('ru-RU', { maximumFractionDigits: 6 }) : '—'} USDT
        </div>
      </div>
      <div style={box}>
        <div style={head}>Порог по позиции — трёхзонное правило от комиссии (Кривые §6.4)</div>
        <div>|обязательство| = <b>{kusd(b.absC)}</b> vs порог входа Z̄lim = <b>{kusd(b.zLim != null ? b.zLim : b.q)}</b> ⇒ избыток = <b>{kusd(b.excess)}</b>{'  '}
          <span style={{ padding: '1px 8px', borderRadius: 10, fontWeight: 600, color: b.breached ? '#f0b0b0' : '#8fe0b0', background: b.breached ? '#5a2020' : '#1f3a2a', border: `1px solid ${b.breached ? '#7a3030' : '#2a4a3a'}` }}>
            {b.breached ? 'ПОРОГ ПРЕВЫШЕН' : 'в полосе'}</span>
        </div>
        {b.zMkt != null && (
          <div style={{ fontSize: 12, color: MUTE, marginTop: 3 }}>
            зоны: ≤{kusd(b.zLim)} — не хеджируем; {kusd(b.zLim)}…{kusd(b.zMkt)} — пассивный <b>мейкер</b>-лимит; &gt;{kusd(b.zMkt)} — агрессивный <b>тейкер</b>-сброс.{' '}
            текущая зона: <b style={{ color: b.aggressive ? '#e6a15a' : '#8fb0e0' }}>{b.aggressive ? 'тейкер (агрессив)' : 'мейкер (пассив)'}</b>
            {!isT && ' · арбитражёр: порог на ногу (ADR-069, rt=1)'}
          </div>
        )}
      </div>
      {vol && (
        <div style={box}>
          <div style={head}>Волатильность σ актива → динамический порог Γ=γσ²τ (T-F18-704)</div>
          <div>σ ({vol.symbol}) = <b>{Number(vol.sigma).toExponential(3)}</b> · выборок <b>{vol.samples}</b> ·{' '}
            свежесть <b style={{ color: vol.ageMs > 120000 ? '#e6a15a' : '#8fe0b0' }}>{(vol.ageMs / 1000).toFixed(1)} с назад</b></div>
          <div style={{ fontSize: 12, color: MUTE, marginTop: 3 }}>
            множитель scale = clamp((σ_ref/σ)²/γ, 0.1, 10) = <b>{Number(vol.scale).toFixed(2)}×</b>{' '}
            (σ_ref={Number(vol.sigmaRef).toExponential(2)}, γ={vol.gammaCap}) · выше σ ⇒ уже полоса (§6.4)
          </div>
          <div style={{ fontSize: 12, marginTop: 3 }}>
            порог при включённом флаге: Z̄lim <b>{kusd(vol.zLimDynamic)}</b> / Z̄mkt <b>{kusd(vol.zMktDynamic)}</b>
            <span style={{ color: MUTE }}> vs текущий плоский {kusd(b.zLim)} / {kusd(b.zMkt)}</span>
          </div>
          <div style={{ fontSize: 11, color: MUTE, marginTop: 3 }}>{vol.note}</div>
        </div>
      )}
      {b.breached && (
        <div style={box}>
          <div style={head}>Хедж-заявка → публичные сделки → имитируемое исполнение</div>
          {h ? (
            <>
              <div>Заявка (пара): <b>{h.pair}</b> <b>{h.side}</b> target <b>{num(h.targetQty)}</b> @ лимит {num(h.limitPrice, 2)}</div>
              {/* Разные времена (живые): когда запрошена лента, когда пришёл ответ, задержка,
                  и насколько стар сам снимок хеджа. Возраст сделок — в колонке таблицы ниже. */}
              <div style={{ marginTop: 6, fontSize: 12, background: '#0c141b', border: '1px solid #1c2733', borderRadius: 6, padding: '6px 8px' }}>
                <div style={{ fontWeight: 600, color: '#cdd9e4', marginBottom: 3 }}>Времена (обновляются вживую)</div>
                <div>• запрос ленты по REST: <b>{fmtAgo(agoMs(h.readRequestMs))}</b></div>
                <div>• ответ по ленте получен: <b>{fmtAgo(agoMs(h.readResponseMs))}</b>{' '}
                  <span style={{ color: MUTE }}>(задержка ответа биржи: {h.readResponseMs && h.readRequestMs ? fmtMs(h.readResponseMs - h.readRequestMs) : '—'})</span></div>
                <div>• снимок (симуляция хеджа) сделан: <b style={{ color: (agoMs(h.createdMs) || 0) > 15000 ? '#e6a15a' : '#8fe0b0' }}>{fmtAgo(agoMs(h.createdMs))}</b>{' '}
                  <span style={{ color: MUTE }}>— это застывший снимок; возраст сделок ниже — живой</span></div>
              </div>
              <div style={{ marginTop: 4 }}>Ближайшие по цене публичные сделки вокруг лимита (контекст последних 60, объём пересекающих по цене = <b>{num(h.crossVol)}</b>). «Исполнила бы» — по цене; фактический матч идёт по свежему окну (см. причину ниже).</div>
              {(() => {
                const L = Number(h.limitPrice) || 0;
                const isSell = h.side === 'SELL';
                const ts = (h.considered || []).map((t) => ({ price: Number(t.price), qty: Number(t.qty), age_ms: t.age_ms, exchange_ms: t.exchange_ms }));
                const above = ts.filter((t) => t.price > L).sort((a, b) => a.price - b.price).slice(0, 5).reverse();  // 5 ближайших выше, сверху выше цена
                const below = ts.filter((t) => t.price < L).sort((a, b) => b.price - a.price).slice(0, 5);            // 5 ближайших ниже
                // Для SELL исполняют сделки ВЫШЕ лимита; для BUY — НИЖЕ. Помечаем сторону.
                const aboveFills = isSell, belowFills = !isSell;
                const tgt = Number(h.targetQty) || 0;
                // Бар объёма — доля от макс. объёма среди строк (хедж + сделки), чтобы
                // визуально сравнить объём заявки и публичных сделок (#2) в ранжировании по цене.
                const maxQty = Math.max(tgt, ...above.map((t) => t.qty), ...below.map((t) => t.qty), 1e-12);
                const bar = (qty, color) => (
                  <div style={{ position: 'relative', minWidth: 96 }}>
                    <div style={{ position: 'absolute', left: 0, top: 2, bottom: 2, width: `${Math.min(100, 100 * qty / maxQty)}%`, background: color, opacity: 0.4, borderRadius: 2 }} />
                    <span style={{ position: 'relative' }}>{qty.toFixed(6)}</span>
                  </div>
                );
                const row = (t, i, fills) => (
                  <tr key={i} style={{ background: fills ? 'rgba(95,208,138,0.08)' : 'rgba(230,114,90,0.07)' }}>
                    <td className="vc-mono">{t.price.toFixed(2)}</td>
                    <td className="vc-mono">{bar(t.qty, fills ? GREEN : RED)}</td>
                    <td className="vc-mono">{t.exchange_ms ? fmtAgo(agoMs(t.exchange_ms)) : (t.age_ms + ' мс')}</td>
                    <td style={{ color: fills ? GREEN : MUTE }}>{fills ? '✓ исполнила бы' : '—'}</td>
                  </tr>
                );
                return (
                  <table className="vc-sub-table" style={{ marginTop: 4 }}>
                    <thead><tr><th>цена</th><th>объём (бар)</th><th>возраст сделки (живой)</th><th>vs лимит</th></tr></thead>
                    <tbody>
                      {above.length === 0 && <tr><td colSpan={4} style={{ opacity: 0.5 }}>нет публичных сделок выше лимита</td></tr>}
                      {above.map((t, i) => row(t, 'a' + i, aboveFills))}
                      <tr style={{ background: '#24384a', color: '#e6c15a', fontWeight: 700 }}>
                        <td className="vc-mono" style={{ color: '#e6c15a' }}>{L.toFixed(2)}</td>
                        <td className="vc-mono">{bar(tgt, '#e6c15a')}</td>
                        <td>◀ ХЕДЖ {h.side}</td><td>лимит заявки</td>
                      </tr>
                      {below.map((t, i) => row(t, 'b' + i, belowFills))}
                      {below.length === 0 && <tr><td colSpan={4} style={{ opacity: 0.5 }}>нет публичных сделок ниже лимита</td></tr>}
                    </tbody>
                  </table>
                );
              })()}
              <div style={{ marginTop: 6 }}>Имитируемое исполнение:{' '}
                <b style={{ color: h.outcome === 'full' ? GREEN : h.outcome === 'partial' ? '#e6c15a' : RED }}>
                  {h.outcome === 'full' ? 'ПОЛНОЕ (filled ≥ target)' : h.outcome === 'partial' ? 'ЧАСТИЧНОЕ (0 < filled < target)' : 'НЕ ИСПОЛНЕНО'}
                </b>{' — '}status {h.status}, filled {num(h.filledQty)}, причина <code>{h.reason}</code>
              </div>
              {h.impactShift != null && h.impactShift !== '' && (
                <div style={{ marginTop: 6, borderTop: '1px dashed #223140', paddingTop: 6, fontSize: 13 }}>
                  <b style={{ color: '#cdd9e4' }}>Price-impact (влияние CE-заявки на рынок):</b>{' '}
                  S (до) = {num(h.baseVwap, 2)} → p_exec = {num(h.avgPrice, 2)};{' '}
                  сдвиг k·v = <b style={{ color: '#e6725a' }}>{num(h.impactShift, 6)}</b>,{' '}
                  издержки k·v²·Δt = <b style={{ color: '#e6725a' }}>{num(h.impactCost, 6)}</b>{' '}
                  (v = {Number(h.impactV || 0).toFixed(4)} лот/с, Δt = {Number(h.impactDtSec || 0).toFixed(2)} с)
                </div>
              )}
            </>
          ) : <div className="vc-note vc-note-tight">хедж-заявок по этому агенту пока нет в диагностике (порог только что превышен либо сделок не было).</div>}
        </div>
      )}
      {isT && !b.breached && <div className="vc-note vc-note-tight">порог не превышен — хедж-заявка не формируется.</div>}
    </div>
  );
}

// (3) диагностика симуляции fill хедж-заявок (реальные заявки + публичные сделки).
function ClearingDetail({ d, onChartPauseChange }) {
  const src = Array.isArray(d.source) ? d.source : [];
  const prices = Array.isArray(d.clearingPrices) ? d.clearingPrices : [];
  const rates = Array.isArray(d.clearingRates) ? d.clearingRates : [];
  // ADR-060: реальные хедж-заявки + рассматриваемые публичные исполнения venue +
  // причина (почему симулятор исполнил / не исполнил). Реальный DTO из
  // venue_fill_diagnostics через BFF. Заменяет синтетические черновики.
  const fills = Array.isArray(d.fillDiagnostics) ? d.fillDiagnostics : [];
  // F-18 v2 (наблюдаемость такта клиринга, ADR-061/063): позиции CE-агентов
  // (переводчики/арбитражёры) ИМЕННО ДЛЯ ЭТОГО такта (batch_id) — ДО→Δ→ПОСЛЕ,
  // из ledger.GetAgentPositionDeltas (реальный DTO, ring-история на стороне
  // ledger, без фейков). Группировка: сначала переводчики, затем
  // арбитражёры, затем прочее; внутри группы — по agent_id/активу.
  // Money-path строки агентов из BFF: одна строка на агента, позиция и Δ такта
  // РАЗЛОЖЕНЫ на ДВЕ НОГИ пары (переводчик: base−/quote+ или наоборот; арбитражёр:
  // одна нога — перевозимый актив). Мердж + домен-математика в BFF (не в браузере).
  const agentRows = (Array.isArray(d.agentRows) ? d.agentRows : [])
    .filter((a) => Math.abs(Number(a.c_total != null ? a.c_total : a.c_position)) > 1e-9 || a.hasDelta);
  const twoSided = src.some((s) => s.twoSided);
  const [chartKey, setChartKey] = useState(null);

  // Только раскрытие строки. Запрос кривой и все вычисления — внутри LiquidityChart
  // (он сам дёргает backend и рисует). Здесь никаких fetch/вычислений.
  const toggleChart = (s) => {
    const key = s.instrument + '|' + s.exchange;
    setChartKey((cur) => (cur === key ? null : key));
  };
  return (
    <div className="vc-detail">
      {/* (1) Исходные данные для клиринга — ОДНА двусторонняя кривая на венью */}
      <div className="vc-sec">
        <div className="vc-sec-title">1. Ликвидность венью — <b>{src.length}</b> {twoSided ? 'двусторонних кривых' : 'сегм.'}{twoSided ? ' · клик по строке → график (стаканы + наклон)' : ''}</div>
        <div className="vc-sec-body">
          {twoSided ? (
            <>
            <table className="vc-sub-table">
              <thead>
                <tr><th>инстр.</th><th>биржа</th><th>mid</th><th title="log-наклон m = mid/(10⁴·α_T)">наклон m</th><th title="ADR-053: линейный β_T = mid·m (safe-translator)">β_T</th><th title="F05A_TRANSLATOR_MODEL">модель</th><th title="глубина покупки = Q_ask">покупка</th><th title="глубина продажи = Q_bid">продажа</th><th>q_rate</th></tr>
              </thead>
              <tbody>
                {src.length === 0 && <tr><td colSpan={9} className="vc-empty">нет кривых</td></tr>}
                {src.map((s, i) => {
                  const key = s.instrument + '|' + s.exchange;
                  const open = chartKey === key;
                  return (
                    <React.Fragment key={s.segment_id || i}>
                      <tr className={`vc-curve-row ${open ? 'vc-curve-open' : ''}`}
                        onClick={() => toggleChart(s)} title="клик — график ликвидности (стаканы + наклон)">
                        <td><span className="vc-caret">{open ? '▾' : '▸'}</span> {s.instrument}</td>
                        <td>{s.exchange}</td>
                        <td className="vc-mono">{fmtNum(s.mid)}</td>
                        <td className="vc-mono vc-slope">{fmtNum(s.slope)}</td>
                        <td className="vc-mono vc-slope">{fmtNum(s.betaT)}</td>
                        <td className="vc-mono">{s.translatorModel || '—'}</td>
                        <td className="vc-mono vc-side-ask">{fmtNum(s.buyDepth)}</td>
                        <td className="vc-mono vc-side-bid">{fmtNum(s.sellDepth)}</td>
                        <td className="vc-mono">{fmtNum(s.speed)}</td>
                      </tr>
                      {open && (
                        <tr>
                          <td colSpan={9} className="vc-chart-cell">
                            <LiquidityChart venue={s.exchange} symbol={s.instrument} ts={d.event_time_ms} onPauseChange={onChartPauseChange} />
                          </td>
                        </tr>
                      )}
                    </React.Fragment>
                  );
                })}
              </tbody>
            </table>
            <div className="vc-note vc-note-tight">
              <b>наклон m</b> = mid/(10⁴·α_T), <b>β_T</b> = mid·m (ADR-053 safe-translator: VWAP-глубина
              + θ-haircut) · <b>покупка</b> = Q_ask · <b>продажа</b> = Q_bid · <b>q_rate</b> = Q_ask+Q_bid.
              Клик по строке — график (raw book + VWAP + safe-линия).
            </div>
            </>
          ) : (
            <table className="vc-sub-table">
              <thead>
                <tr><th>инструмент</th><th>биржа</th><th>сторона</th><th>скорость (q_rate)</th><th>цена (effective)</th><th>q_max</th></tr>
              </thead>
              <tbody>
                {src.length === 0 && <tr><td colSpan={6} className="vc-empty">нет сегментов</td></tr>}
                {src.map((s, i) => (
                  <tr key={s.segment_id || i}>
                    <td>{s.instrument}</td>
                    <td>{s.exchange}</td>
                    <td className={String(s.side).toLowerCase().indexOf('bid') >= 0 ? 'vc-side-bid' : 'vc-side-ask'}>{s.side}</td>
                    <td className="vc-mono">{s.speed}</td>
                    <td className="vc-mono">{s.price}</td>
                    <td className="vc-mono">{s.q_max}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      </div>

      {/* (2) Результат клиринга — клиринговые курсы (реальные) + λ (log-ценности) */}
      <div className="vc-sec">
        <div className="vc-sec-title">2. Клиринговые курсы (реальные) и λ = log(price) по активам</div>
        <div className="vc-sec-body">
          {d.clearingPricesAvailable && prices.length > 0 ? (
            <div className="vc-prices">
              {rates.length > 0 && (
                <div className="vc-rates">
                  {rates.map((r, i) => (
                    <span className="vc-rate-chip" key={r.instrument || i}>
                      {r.instrument}: <b>{r.rate}</b>
                    </span>
                  ))}
                </div>
              )}
              <table className="vc-sub-table">
                <thead><tr><th>актив</th><th>λ = log(price) актива (дуальная Wx=0)</th><th title="exp(λ) — цена актива в неявном numéraire">цена = exp(λ)</th></tr></thead>
                <tbody>
                  {prices.map((p, i) => (
                    <tr key={p.asset || i}>
                      <td>{p.asset}</td>
                      <td className="vc-mono vc-slope">{p.price}</td>
                      <td className="vc-mono">{Number.isFinite(Number(p.priceReal))
                        ? (Math.abs(Number(p.priceReal)) >= 1
                            ? Number(p.priceReal).toFixed(4)
                            : Number(p.priceReal).toPrecision(4))
                        : '—'}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
              <div className="vc-note vc-note-tight">
                ADR-053: клиринг в лог-ценах. λ — это <b>log(цена актива)</b> (дуальные Wx=0,
                определены с точностью до нормировки). Курс инструмента (чипы выше, реальный) =
                <b> exp(λ[quote] − λ[base])</b> = quote/base — разность логов = деление реальных цен,
                поэтому согласовано на циклах (треугольный арбитраж).
              </div>
            </div>
          ) : (
            <div className="vc-note">
              pi (равновесные цены по активам) пока не эмитятся солвером для этого клиринга —
              значение появится после включения расчёта pi в matching. residual_norm ={' '}
              <span className="vc-mono">{d.residual_norm}</span>.
            </div>
          )}
        </div>
      </div>

      {/* (3) Хедж-заявки → внешняя биржа и ближайшие публичные исполнения симулятора.
          Реальные данные (venue_fill_diagnostics): заявка + рассматриваемые публичные
          сделки venue + причина, почему симулятор исполнил / не исполнил (ADR-060). */}
      <div className="vc-sec">
        <div className="vc-sec-title">3. Хедж-заявки → биржа и публичные исполнения симулятора (почему fill) — <b>{fills.length}</b></div>
        <div className="vc-sec-body">
          {fills.length > 0 ? (
            <table className="vc-sub-table">
              <thead>
                <tr>
                  <th>вид</th><th>биржа</th><th>символ</th><th>сторона</th>
                  <th>target</th><th>лимит</th><th>исполнено</th><th>ср.цена</th>
                  <th>статус</th><th>причина</th><th>публичные сделки окна</th>
                </tr>
              </thead>
              <tbody>
                {fills.map((f, i) => (
                  <tr key={f.intentId || i} style={{ background: reasonBg(f.reason) }}>
                    <td>{f.kind === 'band' ? 'полоса' : 'клиринг'}</td>
                    <td>{f.venue}</td>
                    <td className="vc-mono">{f.pair || f.symbol}</td>
                    <td className={f.side === 'SELL' ? 'vc-side-bid' : 'vc-side-ask'}>{f.side}</td>
                    <td className="vc-mono">{f.targetQty}</td>
                    <td className="vc-mono">{f.limitPrice}</td>
                    <td className="vc-mono">{f.filledQty}</td>
                    <td className="vc-mono">{f.avgPrice}</td>
                    <td>{f.status}</td>
                    <td title={f.reason}>{reasonLabel(f.reason)}</td>
                    <td>
                      <details>
                        <summary className="vc-mono" style={{ cursor: 'pointer' }}>
                          {f.windowTrades}{(f.consideredTrades || []).length ? '' : ' (пусто)'}
                        </summary>
                        {(f.consideredTrades || []).length > 0 && (
                          <table className="vc-sub-table" style={{ marginTop: 4 }}>
                            <thead>
                              <tr><th>цена</th><th>объём</th><th>возраст, мс</th><th>пересекает лимит</th></tr>
                            </thead>
                            <tbody>
                              {f.consideredTrades.map((t, j) => (
                                <tr key={j} style={{ background: t.crosses ? '#e7f6e7' : 'transparent' }}>
                                  <td className="vc-mono">{t.price}</td>
                                  <td className="vc-mono">{t.qty}</td>
                                  <td className="vc-mono">{t.age_ms}</td>
                                  <td>{t.crosses ? '✓ да' : '— нет'}</td>
                                </tr>
                              ))}
                            </tbody>
                          </table>
                        )}
                      </details>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          ) : (
            <div className="vc-note">
              Пока нет диагностики sim-fill (venue пишет в <span className="vc-mono">venue_fill_diagnostics</span>,
              когда обрабатывает хедж-заявку). Частый случай отказа: band-заявка без лимит-цены →
              «нет лимит-цены → симулятор пропускает матчинг».
            </div>
          )}
        </div>
      </div>

      {/* (4) Позиции агентов ПО ПАРАМ ВАЛЮТ (money-path): одна строка на агента,
          позиция и Δ такта разложены на ДВЕ НОГИ пары (переводчик: base−/quote+ при
          SELL, наоборот при BUY; арбитражёр: одна нога — перевозимый актив). Ноги
          считает BFF из c_j и цены пары (реальный DTO, без фейков; не в браузере). */}
      <AgentPositionsTables agentRows={agentRows} pnlTotals={d.pnlTotals} title="4. Позиции агентов по парам валют (money-path)" />
    </div>
  );
}

function VectorClearingLive() {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [items, setItems] = useState([]);
  const [summary, setSummary] = useState({});
  const [total, setTotal] = useState(0);
  const [error, setError] = useState('');
  const [updatedAt, setUpdatedAt] = useState(null);
  const [dataAgeMs, setDataAgeMs] = useState(null);  // возраст ДАННЫХ клиринга (heartbeat цепочки)
  const [stages, setStages] = useState(null);        // постадийная свежесть цепочки (/liveness)
  const [liveOpen, setLiveOpen] = useState(false);   // панель ЖИВЫХ позиций агентов (динамика)
  const [liveRows, setLiveRows] = useState(null);    // agentRows живого просмотра
  const [livePnlTotals, setLivePnlTotals] = useState(null);  // итоги заработка из ledger (по видам и общий) — панель их только показывает
  const [liveAt, setLiveAt] = useState(null);        // время последнего обновления живой панели
  const [selAgent, setSelAgent] = useState(null);    // выбранный агент для drill-down
  const [agentDetail, setAgentDetail] = useState(null); // /agent-detail выбранного агента
  const [openKey, setOpenKey] = useState(null);
  const [chartPaused, setChartPaused] = useState(false);  // пауза графика ⇒ заморозить и список (строки не двигаются/не исчезают)
  const [detailByKey, setDetailByKey] = useState({});
  const [detailErr, setDetailErr] = useState('');
  // Runtime-конфиг цикла батч-клиринга (окно/staleness).
  const [cfgWindowMs, setCfgWindowMs] = useState('');
  const [cfgStaleMs, setCfgStaleMs] = useState('');
  const [cfgFeeBps, setCfgFeeBps] = useState('');   // комиссия тейкера, bps (0 = линейно)
  const [cfgSkewGamma, setCfgSkewGamma] = useState('');   // inventory-skew γ (‰/k-USDT, 0=выкл)
  const [cfgSkewMaxPm, setCfgSkewMaxPm] = useState('');   // клэмп смещения (‰)
  const [cfgSlopeMethod, setCfgSlopeMethod] = useState('4'); // §6.2 способ наклона (1..4) — глобально
  const [cfgBandK, setCfgBandK] = useState('');     // порог хеджа k_band (q = k·φ_rt); 0 = плоский env-порог
  const [cfgMakerBps, setCfgMakerBps] = useState(''); // мейкер clim (bps) — пассивная зона Z̄lim
  // ADR-081: режим ширины полосы хеджа — 'fee' (от комиссии), 'volatility' (авто по σ), 'manual' (вручную).
  const [cfgBandMode, setCfgBandMode] = useState('fee');
  const [cfgManualLim, setCfgManualLim] = useState('');   // Z̄lim вручную, тыс. USDT
  const [cfgManualMkt, setCfgManualMkt] = useState('');   // Z̄mkt вручную, тыс. USDT
  const [cfgBandFloor, setCfgBandFloor] = useState('');   // пол ширины, тыс. USDT
  const [cfgSigmaRef, setCfgSigmaRef] = useState('');     // опорная σ
  const [cfgVrMin, setCfgVrMin] = useState('');           // нижняя граница множителя волатильности
  const [cfgVrMax, setCfgVrMax] = useState('');           // верхняя граница множителя волатильности
  const [cfgArbScale, setCfgArbScale] = useState('1');    // множитель порога выхода арбитражёров (ADR-081)
  const [cfgMsg, setCfgMsg] = useState('');
  const [cfgLoaded, setCfgLoaded] = useState(false);   // настройки прочитаны с сервера; до этого «Применить» заблокировано
  const [cfgSaving, setCfgSaving] = useState(false);
  const [resetMsg, setResetMsg] = useState('');   // #3 фидбек сброса позиций
  const [resetting, setResetting] = useState(false);

  const loadConfig = useCallback(async () => {
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/config`, { timeout: 8000 });
      setCfgWindowMs(String(r.data.batch_window_ms ?? 1000));
      setCfgStaleMs(String(r.data.stale_level_ms ?? 60000));
      setCfgFeeBps(String(r.data.ce_taker_fee_bps ?? -1));
      setCfgSkewGamma(String(r.data.ce_inv_skew_gamma ?? 0.02));
      setCfgSkewMaxPm(String(r.data.ce_inv_skew_max_pm ?? 8));
      setCfgSlopeMethod(String(r.data.slope_method ?? 4));
      setCfgBandK(String(r.data.ce_band_fee_k ?? 1.8));
      setCfgMakerBps(String(r.data.ce_maker_fee_bps ?? 2));
      setCfgBandMode(r.data.ce_band_mode || 'fee');
      setCfgManualLim(String(r.data.ce_band_manual_q_lim ?? 5));
      setCfgManualMkt(String(r.data.ce_band_manual_q_mkt ?? 25));
      setCfgBandFloor(String(r.data.ce_band_q_floor ?? 5));
      setCfgSigmaRef(String(r.data.ce_band_sigma_ref ?? 0.0005));
      setCfgVrMin(String(r.data.ce_band_vol_ratio_min ?? 0.1));
      setCfgVrMax(String(r.data.ce_band_vol_ratio_max ?? 10));
      setCfgArbScale(String(r.data.ce_band_arb_scale ?? 1));
      setCfgLoaded(true);
    } catch (e) { /* PG может быть недоступен — оставляем пустым */ }
  }, []);

  const saveConfig = useCallback(async () => {
    setCfgSaving(true); setCfgMsg('');
    try {
      const r = await axios.post(`${API_BASE}/vector-clearing/config`, {
        batch_window_ms: Number(cfgWindowMs), stale_level_ms: Number(cfgStaleMs),
        ce_taker_fee_bps: Number(cfgFeeBps),
        ce_inv_skew_gamma: Number(cfgSkewGamma), ce_inv_skew_max_pm: Number(cfgSkewMaxPm),
        slope_method: parseInt(cfgSlopeMethod, 10),
        ce_band_fee_k: Number(cfgBandK), ce_maker_fee_bps: Number(cfgMakerBps),
        // ADR-081: режим ширины полосы. Ручной включает ручные границы; авто по волатильности — режим σ и его параметры.
        ce_band_manual: cfgBandMode === 'manual',
        ...(cfgBandMode === 'manual'
          ? { ce_band_manual_q_lim: Number(cfgManualLim), ce_band_manual_q_mkt: Number(cfgManualMkt) }
          : { ce_band_gamma_mode: cfgBandMode === 'volatility', ce_band_arb_scale: Number(cfgArbScale) }),
        ...(cfgBandMode === 'volatility'
          ? { ce_band_q_floor: Number(cfgBandFloor), ce_band_sigma_ref: Number(cfgSigmaRef),
              ce_band_vol_ratio_min: Number(cfgVrMin), ce_band_vol_ratio_max: Number(cfgVrMax) }
          : {})
      }, { timeout: 8000 });
      setCfgWindowMs(String(r.data.batch_window_ms));
      setCfgStaleMs(String(r.data.stale_level_ms));
      setCfgFeeBps(String(r.data.ce_taker_fee_bps));
      setCfgSkewGamma(String(r.data.ce_inv_skew_gamma));
      setCfgSkewMaxPm(String(r.data.ce_inv_skew_max_pm));
      setCfgSlopeMethod(String(r.data.slope_method));
      setCfgBandK(String(r.data.ce_band_fee_k));
      setCfgMakerBps(String(r.data.ce_maker_fee_bps));
      if (r.data.ce_band_mode) setCfgBandMode(r.data.ce_band_mode);
      setCfgMsg('применено ✓ (ledger/matching/market_data/venues подхватят ≤3с)');
    } catch (e) {
      const serverMsg = e && e.response && e.response.data && e.response.data.message;
      setCfgMsg('ошибка: ' + (serverMsg || e.message || 'не сохранено'));
    } finally { setCfgSaving(false); }
  }, [cfgWindowMs, cfgStaleMs, cfgFeeBps, cfgSkewGamma, cfgSkewMaxPm, cfgSlopeMethod, cfgBandK, cfgMakerBps,
      cfgBandMode, cfgManualLim, cfgManualMkt, cfgBandFloor, cfgSigmaRef, cfgVrMin, cfgVrMax, cfgArbScale]);

  const rowKey = (it) => `${it.batch_id}|${it.event_time_ms}`;

  // Загрузка детали батча. Всегда обновляет кэш (позиция/агенты «догоняют» по мере
  // применения ledger — свежий батч отдаёт позицию с лагом 1-3с).
  const fetchDetail = useCallback(async (it) => {
    const key = rowKey(it);
    try {
      const resp = await axios.get(`${API_BASE}/vector-clearing/detail`, {
        params: { batch_id: it.batch_id, ts: it.event_time_ms }, timeout: 12000
      });
      setDetailByKey((m) => ({ ...m, [key]: resp.data }));
      setDetailErr('');
    } catch (e) {
      setDetailErr(e.message || 'ошибка загрузки детали');
    }
  }, []);

  const toggleRow = useCallback(async (it) => {
    const key = rowKey(it);
    if (openKey === key) { setOpenKey(null); return; }
    setOpenKey(key);
    setDetailErr('');
    await fetchDetail(it);   // тянем СНИМОК батча один раз (деталь статична, без авто-рефетча)
  }, [openKey, fetchDetail]);

  useEffect(() => {
    (async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    })();
  }, [navigate]);

  const load = useCallback(async () => {
    try {
      const response = await axios.get(`${API_BASE}/vector-clearing/live`, { timeout: 10000 });
      const data = response.data || {};
      setItems(Array.isArray(data.items) ? data.items : []);
      setSummary(data.summary || {});
      setTotal(Number(data.total) || 0);
      setUpdatedAt(new Date());
      setDataAgeMs(Number.isFinite(Number(data.dataAgeMs)) ? Number(data.dataAgeMs) : null);
      setError('');
    } catch (e) {
      setError(e.message || 'ошибка загрузки');
    }
  }, []);

  const loadLiveness = useCallback(async () => {
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/liveness`, { timeout: 10000 });
      setStages(Array.isArray(r.data && r.data.stages) ? r.data.stages : null);
    } catch (e) { /* эндпоинт недоступен — полоса стадий скрыта */ }
  }, []);

  // Живые позиции агентов (динамическая панель по кнопке): ТЕКУЩИЕ позиции всех
  // агентов + дельты свежего такта. Независимо от выбранного статичного батча.
  const loadLivePositions = useCallback(async () => {
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/agent-positions`, { timeout: 10000 });
      const all = Array.isArray(r.data && r.data.agentRows) ? r.data.agentRows : [];
      // Все агенты сразу (в том числе не действующие в этом такте) и без исчезновений между опросами: порядок строк задаёт
      // сортировка по биржам (agentRowsLayout), а не ход клиринга.
      setLiveRows((prev) => mergeStableRows(prev, all));
      setLivePnlTotals((r.data && r.data.pnl_totals) || null);
      setLiveAt(new Date());
    } catch (e) { /* недоступно — панель покажет прошлое/пусто */ }
  }, []);

  // Drill-down выбранного агента (цена/порог/хедж/исполнение) — тоже вживую.
  const loadAgentDetail = useCallback(async (agentId) => {
    if (!agentId) { setAgentDetail(null); return; }
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/agent-detail`, { params: { agent_id: agentId }, timeout: 10000 });
      setAgentDetail(r.data || null);
    } catch (e) { /* агент мог исчезнуть — оставляем прошлое */ }
  }, []);

  const selectAgent = useCallback((agentId) => {
    setSelAgent((cur) => {
      const next = cur === agentId ? null : agentId;  // повторный клик — снять выбор
      loadAgentDetail(next);
      if (!next) setAgentDetail(null);
      return next;
    });
  }, [loadAgentDetail]);

  // #3 РЕАЛЬНЫЙ сброс позиций агентов в ledger (не фронт-фикс): c_j→0, in_flight→0,
  // band-хеджи очищаются. После — сразу перезагружаем, чтобы видеть рост с нуля.
  const resetPositions = useCallback(async () => {
    if (resetting) return;
    if (!window.confirm('Сбросить ВСЕ позиции агентов к нулю? Это реальный сброс в ledger — позиции начнут копиться заново с текущего клиринга.')) return;
    setResetting(true); setResetMsg('');
    try {
      const r = await axios.post(`${API_BASE}/vector-clearing/reset-positions`, {}, { timeout: 10000 });
      setResetMsg(`сброшено агентов: ${(r.data && r.data.cleared) || 0} ✓ — позиции копятся с нуля`);
      await load();
    } catch (e) {
      setResetMsg('ошибка сброса: ' + (e.message || 'не выполнено'));
    } finally { setResetting(false); }
  }, [resetting, load]);

  useEffect(() => {
    if (isAuth) { load(); loadConfig(); loadLiveness(); }
  }, [isAuth, load, loadConfig, loadLiveness]);

  useInterval(() => {
    // Пауза графика замораживает и список батчей: иначе строки двигаются и открытый
    // батч через ~20-30с уходит из списка → график исчезает. На паузе не грузим.
    if (isAuth && !chartPaused) { load(); loadLiveness(); }
  }, POLL_INTERVAL_MS);

  // Живая панель обновляется только когда открыта (кнопка). Не трогает статичный батч.
  useInterval(() => {
    if (isAuth && liveOpen) {
      loadLivePositions();
      if (selAgent) loadAgentDetail(selAgent);  // drill-down выбранного агента — тоже вживую
    }
  }, POLL_INTERVAL_MS);

  const toggleLive = useCallback(() => {
    setLiveOpen((v) => {
      const next = !v;
      if (next) loadLivePositions();  // сразу подтянуть при открытии
      return next;
    });
  }, [loadLivePositions]);

  // Открытая деталь батча — СТАТИЧНА (снимок выбранного клиринга): тянется один раз
  // при клике (toggleRow → fetchDetail) и НЕ обновляется во время просмотра. Живой
  // просмотр позиций — отдельная кнопка «Живые позиции агентов» (ниже).

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  // Живость цепочки Стаканы→Кривые→Клиринг→Позиции→Хеджи. dataAgeMs — возраст
  // ДАННЫХ (серверные часы − event_time_ms свежего такта), а не время загрузки в
  // браузере. Порог — из runtime-конфига stale_level_ms. Заморозка пайплайна сразу
  // видна: клиринг не эмитит такты → возраст растёт → «ЦЕПОЧКА ОСТАНОВЛЕНА».
  const staleThresholdMs = Math.max(2000, Number(cfgStaleMs) || 60000);
  const chainStale = dataAgeMs != null && dataAgeMs > staleThresholdMs;
  const fmtAge = (ms) => {
    if (ms == null) return '—';
    const s = Math.round(ms / 1000);
    if (s < 90) return `${s} с`;
    const m = Math.round(s / 60);
    if (m < 90) return `${m} мин`;
    const h = Math.round(m / 60);
    if (h < 48) return `${h} ч`;
    return `${Math.round(h / 24)} дн`;
  };

  return (
    <div className="vc-page">
      <NavBar />
      <div className="vc-content">
        {dataAgeMs != null && (
          <div
            className={`vc-liveness ${chainStale ? 'vc-liveness--stale' : 'vc-liveness--live'}`}
            role="status"
            style={{
              display: 'flex', alignItems: 'center', gap: 10,
              padding: '10px 14px', marginBottom: 12, borderRadius: 8,
              fontWeight: 600,
              border: `1px solid ${chainStale ? '#c0392b' : '#1e8449'}`,
              background: chainStale ? 'rgba(192,57,43,0.12)' : 'rgba(30,132,73,0.10)',
              color: chainStale ? '#c0392b' : '#1e8449'
            }}
          >
            <span style={{ fontSize: 18, lineHeight: 1 }}>{chainStale ? '⚠' : '●'}</span>
            {chainStale ? (
              <span>
                ЦЕПОЧКА ОСТАНОВЛЕНА — последний клиринг <b>{fmtAge(dataAgeMs)}</b> назад
                (порог {fmtAge(staleThresholdMs)}). Данные историчны, не живой процесс.
                Проверьте venues/market_data/matching.
              </span>
            ) : (
              <span>
                ЖИВОЙ ПРОЦЕСС — Стаканы → Кривые → Клиринг → Позиции → Хеджи.
                Последний такт <b>{fmtAge(dataAgeMs)}</b> назад.
              </span>
            )}
          </div>
        )}
        {Array.isArray(stages) && stages.length > 0 && (
          <div style={{
            display: 'flex', alignItems: 'center', flexWrap: 'wrap', gap: 6,
            padding: '8px 12px', marginBottom: 12, borderRadius: 8,
            border: '1px solid #d0d7de', background: 'rgba(127,127,127,0.06)'
          }}>
            {stages.map((s, i) => {
              const noData = s.ageMs == null;
              const bad = s.stale === true;
              const col = noData ? '#8a8a8a' : (bad ? '#c0392b' : '#1e8449');
              return (
                <span key={s.key} style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
                  {i > 0 && <span style={{ color: '#8a8a8a', fontWeight: 700 }}>→</span>}
                  <span
                    title={noData ? 'нет данных стадии' : `свежесть ${fmtAge(s.ageMs)}`}
                    style={{
                      display: 'inline-flex', alignItems: 'center', gap: 5,
                      padding: '3px 9px', borderRadius: 14, fontSize: 12.5, fontWeight: 600,
                      border: `1px solid ${col}`, color: col,
                      background: noData ? 'transparent' : (bad ? 'rgba(192,57,43,0.10)' : 'rgba(30,132,73,0.08)')
                    }}
                  >
                    <span style={{ fontSize: 9 }}>{noData ? '○' : (bad ? '⚠' : '●')}</span>
                    {s.label}
                    <b>{noData ? '—' : fmtAge(s.ageMs)}</b>
                  </span>
                </span>
              );
            })}
          </div>
        )}
        <div className="vc-header">
          <div>
            <h1 className="vc-title">Vector Clearing <span className="vc-sub">— F-05A live</span></h1>
            <div className="vc-desc">
              market_data векторизует внешнюю ликвидность → matching решает QP <code>Wx=0</code> (OSQP) + surplus
            </div>
          </div>
          <div className="vc-summary">
            <span className="vc-chip">всего: <b>{total}</b></span>
            <span className="vc-chip s-converged">converged: <b>{summary.converged || 0}</b></span>
            <span className="vc-chip s-degraded">degraded: <b>{summary.degraded || 0}</b></span>
            <span className="vc-chip s-failed">failed: <b>{summary.failed || 0}</b></span>
            <span className="vc-chip vc-exec-chip">executed: <b>{summary.executed || 0}</b></span>
            {updatedAt && (
              <span className="vc-updated">обновлено {updatedAt.toLocaleTimeString('ru-RU', { hour12: false })}</span>
            )}
            <button
              onClick={toggleLive}
              title="Динамический просмотр позиций агентов (обновляется вживую), независимо от выбранного батча"
              style={{
                padding: '4px 12px', borderRadius: 6, fontSize: 12.5, fontWeight: 600, cursor: 'pointer',
                border: `1px solid ${liveOpen ? '#1e8449' : '#2563eb'}`,
                color: liveOpen ? '#1e8449' : '#2563eb',
                background: liveOpen ? 'rgba(30,132,73,0.10)' : 'transparent'
              }}
            >
              {liveOpen ? '● Живые позиции: вкл' : '◐ Живые позиции агентов'}
            </button>
            <button
              onClick={resetPositions}
              disabled={resetting}
              title="Реальный сброс c_j→0 в ledger — наблюдать расхождение позиций с нуля"
              style={{
                padding: '4px 12px', borderRadius: 6, fontSize: 12.5, fontWeight: 600,
                cursor: resetting ? 'default' : 'pointer',
                border: '1px solid #c0392b', color: '#c0392b',
                background: resetting ? 'rgba(192,57,43,0.06)' : 'transparent'
              }}
            >
              {resetting ? 'сброс…' : '⟲ Сбросить позиции'}
            </button>
            {resetMsg && <span style={{ fontSize: 12, color: '#1e8449' }}>{resetMsg}</span>}
          </div>
        </div>

        <div className="vc-legend">
          <div className="vc-legend-row">
            Каждая строка — результат клиринга одного снимка внешней ликвидности:
            matching решает баланс <code>W·x = 0</code> по векторным сегментам (OSQP).
          </div>
          <div className="vc-legend-row">
            <b>status</b>: <span className="s-converged">converged</span> — сошёлся, остаток ≈ 0;{' '}
            <span className="s-degraded">degraded</span> — остаток выше допуска;{' '}
            <span className="s-failed">failed</span> — не решился. &nbsp;
            <b>residual_norm</b> = <code>‖W·x‖</code> (0 = идеально сбалансировано). &nbsp;
            <b>leg_count</b> — число сегментов (внешних уровней). &nbsp;
            <b>executed</b> — есть ли фактический объём <code>x&gt;0</code>.
          </div>
          <div className="vc-legend-note">
            <b>executed = нет</b> означает, что решатель не нашёл прибыльного перебаланса
            (ликвидность уже сбалансирована / нет арбитража) — система корректно не создаёт
            фантомных сделок, поэтому это нормальный, а не ошибочный результат.
            <b> executed = да</b> — найден ненулевой поток <code>x&gt;0</code> (появляется при
            кросс-venue / треугольном арбитраже). При включённом флаге <code>F05A_MONEY_ENABLED</code>
            только converged-клиринги с <code>x&gt;0</code> порождают хедж-заявки (F-12).
          </div>
        </div>

        <div className="vc-config">
          <span className="vc-config-title">Цикл батч-клиринга:</span>
          <label className="vc-config-field">
            окно (мс)
            <input type="number" min="100" max="600000" step="500" value={cfgWindowMs}
              onChange={(e) => setCfgWindowMs(e.target.value)} />
          </label>
          <label className="vc-config-field">
            staleness (мс)
            <input type="number" min="100" max="3600000" step="1000" value={cfgStaleMs}
              onChange={(e) => setCfgStaleMs(e.target.value)} />
          </label>
          <label className="vc-config-field" title="Комиссия тейкера для мёртвой зоны c = комиссия + ½·spread. 0 = линейные кривые без полки-комиссии; −1 = брать из стакана venue.">
            комиссия (bps)
            <input type="number" min="-1" max="1000" step="0.5" value={cfgFeeBps}
              onChange={(e) => setCfgFeeBps(e.target.value)} />
          </label>
          <label className="vc-config-field" title="Inventory-skew γ: обратная связь позиция→цена. anchor_eff = anchor − clamp(γ·c_j). Больше γ → сильнее возврат позиций к нулю; 0 = выкл (позиции дрейфуют). Крутить осторожно — возможны колебания.">
            skew γ (‰/kU)
            <input type="number" min="0" max="100" step="0.01" value={cfgSkewGamma}
              onChange={(e) => setCfgSkewGamma(e.target.value)} />
          </label>
          <label className="vc-config-field" title="Клэмп смещения скью (‰): максимум |anchor_eff − anchor|. Ограничивает силу возврата (устойчивость).">
            skew клэмп (‰)
            <input type="number" min="0" max="500" step="1" value={cfgSkewMaxPm}
              onChange={(e) => setCfgSkewMaxPm(e.target.value)} />
          </label>
          <label className="vc-config-field" title="Порог хеджирования (band, Кривые §6.4): агент начинает хеджировать при |позиция| > q, где q = k_band·φ_rt (φ_rt — round-trip комиссия внешних бирж в bps; арбитражёр ×2). Больше k ⇒ выше порог ⇒ агент терпит большую позицию до хеджа. 0 = плоский env-порог. ledger поллит (живая настройка).">
            порог хеджа k_band
            <input type="number" min="0" max="100" step="0.1" value={cfgBandK}
              onChange={(e) => setCfgBandK(e.target.value)} />
          </label>
          <label className="vc-config-field" title="Мейкер-комиссия clim (bps) — пассивная зона трёхзонного порога (Кривые §6.4): Z̄lim = k_band·clim·rt (вход в хедж пассив-мейкером). Тейкер-зона Z̄mkt = k_band·(комиссия тейкера)·rt.">
            мейкер clim (bps)
            <input type="number" min="0" max="1000" step="0.5" value={cfgMakerBps}
              onChange={(e) => setCfgMakerBps(e.target.value)} />
          </label>
          <label className="vc-config-field" data-testid="band-mode" data-ui-id="UI-VC-BAND-MODE" title="Как задаётся ширина полосы хеджа — граница |позиции агента|, за которой он отправляет заявку на внешнюю биржу. Вручную: вы задаёте границы напрямую. Авто по волатильности: ширина от комиссии, умноженная на коэффициент по волатильности актива (чем волатильнее, тем уже). От комиссии: ширина от комиссии без поправки на волатильность.">
            ширина полосы хеджа
            <select value={cfgBandMode} onChange={(e) => setCfgBandMode(e.target.value)}>
              <option value="fee">от комиссии (плоская)</option>
              <option value="volatility">авто по волатильности</option>
              <option value="manual">вручную</option>
            </select>
          </label>
          {cfgBandMode !== 'manual' && (
            <label className="vc-config-field" data-testid="arb-scale" title="Во сколько раз порог выхода на внешнюю биржу у АРБИТРАЖЁРОВ шире расчётного (комиссия + плата за перевод). Позиция арбитражёра нейтральна по активу (длинная нога на одной бирже, короткая на другой), рыночного риска нет, поэтому ему можно держать позицию крупнее, чем переводчику, и выходить наружу реже. 1 — без изменения; переводчиков не касается; в ручном режиме не используется.">
              множитель порога арбитражёров
              <input type="number" min="0.1" max="20" step="0.1" value={cfgArbScale}
                onChange={(e) => setCfgArbScale(e.target.value)} />
            </label>
          )}
          {cfgBandMode === 'manual' && (<>
            <label className="vc-config-field" title="Z̄lim: при |позиции| больше этого значения агент хеджируется пассивной лимитной заявкой. Тысячи USDT, одно значение для всех агентов. Больше значение — реже и крупнее хеджи.">
              Z̄lim (тыс. USDT)
              <input type="number" min="0" max="100000" step="0.5" value={cfgManualLim}
                onChange={(e) => setCfgManualLim(e.target.value)} />
            </label>
            <label className="vc-config-field" title="Z̄mkt: при |позиции| больше этого значения агент хеджируется рыночной заявкой (агрессивно). Должно быть не меньше Z̄lim и не больше 100000.">
              Z̄mkt (тыс. USDT)
              <input type="number" min="0" max="100000" step="0.5" value={cfgManualMkt}
                onChange={(e) => setCfgManualMkt(e.target.value)} />
            </label>
          </>)}
          {cfgBandMode === 'volatility' && (<>
            <label className="vc-config-field" title="Нижняя граница ширины полосы (тыс. USDT): даже при очень высокой волатильности полоса не станет уже этого значения.">
              пол (тыс. USDT)
              <input type="number" min="0" max="100000" step="0.5" value={cfgBandFloor}
                onChange={(e) => setCfgBandFloor(e.target.value)} />
            </label>
            <label className="vc-config-field" title="Опорная волатильность σ: при σ актива, равной опорной, ширина равна плоской от комиссии. Выше σ — полоса уже, ниже — шире.">
              опорная σ
              <input type="number" min="0" max="1" step="0.00005" value={cfgSigmaRef}
                onChange={(e) => setCfgSigmaRef(e.target.value)} />
            </label>
            <label className="vc-config-field" title="Границы множителя волатильности: во сколько раз ширина может уменьшиться (мин) и увеличиться (макс) относительно плоской.">
              множитель, мин
              <input type="number" min="0" max="1000" step="0.1" value={cfgVrMin}
                onChange={(e) => setCfgVrMin(e.target.value)} />
            </label>
            <label className="vc-config-field" title="Верхняя граница множителя волатильности.">
              множитель, макс
              <input type="number" min="0" max="1000" step="0.1" value={cfgVrMax}
                onChange={(e) => setCfgVrMax(e.target.value)} />
            </label>
          </>)}
          {(() => {  // превью порога входа Z̄lim (переводчик rt=1) и Z̄mkt из текущих полей
            if (cfgBandMode === 'manual') {
              return <span className="vc-config-hint" title="Ручной режим: комиссия, волатильность и ликвидность на ширину не влияют.">ручная ширина: Z̄lim <b>{cfgManualLim}</b> · Z̄mkt <b>{cfgManualMkt}</b> тыс. USDT (для всех агентов)</span>;
            }
            const k = Number(cfgBandK), clim = Number(cfgMakerBps), cmkt = Number(cfgFeeBps);
            if (!(k > 0) || !(clim >= 0)) return <span className="vc-config-hint">порог: плоский env (k_band=0)</span>;
            const zLim = Math.max(5, k * clim);
            const zMkt = Math.max(zLim, k * (cmkt > 0 ? cmkt : 10));
            return <span className="vc-config-hint" title="Z̄lim — вход в хедж (пассив); Z̄mkt — агрессив-тейкер. Переводчик rt=1; арбитражёр ×2.">≈ порог входа Z̄lim <b>{zLim.toFixed(1)}</b> k-USDT · Z̄mkt <b>{zMkt.toFixed(1)}</b> (переводчик)</span>;
          })()}
          <label className="vc-config-field" title="§6.2 «Кривые котирования»: способ снятия наклона α_ext одного стакана. На основе выбранного варианта считаются ВСЕ кривы ликвидности, клиринг и отображение на фронте. M4 минорант — §6.3-safe (не переобещает глубину, дефолт); M2 МНК точнее трекает VWAP; M1/M3 — касательная/по выгоде. Выбор ≠ минорант меняет клиринговые цены.">
            наклон (§6.2)
            <select value={cfgSlopeMethod} onChange={(e) => setCfgSlopeMethod(e.target.value)}>
              <option value="1">M1 касательная</option>
              <option value="2">M2 МНК (VWAP)</option>
              <option value="3">M3 по выгоде</option>
              <option value="4">M4 минорант (safe)</option>
            </select>
          </label>
          <button className="vc-config-apply" onClick={saveConfig} disabled={cfgSaving || !cfgLoaded} title={cfgLoaded ? '' : 'настройки не загружены с сервера — применять нечего'}>
            {cfgSaving ? '…' : 'Применить'}
          </button>
          {cfgMsg && <span className="vc-config-msg">{cfgMsg}</span>}
          {cfgSlopeMethod !== '4' && <span className="vc-config-msg" style={{ color: '#e0a04a' }} title="§6.3 «правило транслируемой глубины»: только минорант (M4) гарантированно не обещает больше, чем есть в книге.">⚠ наклон ≠ минорант: возможно локальное переобещание глубины (§6.3)</span>}
          <span className="vc-config-hint">наклон §6.2 — глобально: выбранный вариант управляет расчётом всех кривых, клирингом и отображением (venues/market_data подхватят ≤3с). Комиссия 0 = линейные кривые; skew γ&gt;0 = обратная связь позиция→цена.</span>
        </div>

        {/* ЖИВОЙ просмотр позиций агентов (по кнопке) — динамика, независимо от
            выбранного статичного батча. Обновляется каждые POLL_INTERVAL_MS. */}
        {liveOpen && (
          <div style={{ marginBottom: 12, border: '1px solid #1e8449', borderRadius: 8, background: 'rgba(30,132,73,0.04)' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '6px 12px', fontWeight: 600, color: '#1e8449' }}>
              <span>● ЖИВЫЕ позиции агентов — обновляется каждые {Math.round(POLL_INTERVAL_MS / 1000)} с</span>
              {liveAt && <span style={{ fontSize: 12, opacity: 0.7 }}>обновлено {liveAt.toLocaleTimeString('ru-RU', { hour12: false })}</span>}
              <button onClick={toggleLive} style={{ marginLeft: 'auto', padding: '2px 10px', borderRadius: 6, fontSize: 12, cursor: 'pointer', border: '1px solid #888', background: 'transparent' }}>закрыть</button>
            </div>
            {liveRows == null
              ? <div className="vc-note vc-note-tight">загрузка живых позиций…</div>
              : (
                <>
                  <SystemPnlBlock />
                  <AgentPositionsTables agentRows={liveRows} pnlTotals={livePnlTotals}
                    title="Позиции агентов — ЖИВОЙ просмотр (money-path) · клик по строке — разбор агента"
                    onSelectAgent={selectAgent} selectedId={selAgent} />
                </>
              )}
          </div>
        )}

        {/* #3 Разбор агента — модал ЧЕРЕЗ ПОРТАЛ в body (иначе fixed ломается родителем
            с transform и окно уезжает вниз). По центру экрана, без прокрутки. */}
        {selAgent && createPortal((
          <div
            onClick={() => selectAgent(selAgent)}
            style={{
              position: 'fixed', inset: 0, zIndex: 3000, background: 'rgba(0,0,0,0.6)',
              display: 'flex', alignItems: 'center', justifyContent: 'center', padding: '3vh 12px', overflow: 'auto'
            }}
          >
            <div
              onClick={(e) => e.stopPropagation()}
              style={{
                background: '#0b0f14', borderRadius: 10, border: '1px solid #223140', width: 'min(760px, 96vw)',
                maxHeight: '88vh', overflow: 'auto', boxShadow: '0 12px 40px rgba(0,0,0,0.6)', color: '#dfe6ee'
              }}
            >
              <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '10px 14px', fontWeight: 700, color: '#cdd9e4', borderBottom: '1px solid #1c2733', position: 'sticky', top: 0, background: '#1a2531' }}>
                <span>Разбор агента: {selAgent}</span>
                <button onClick={() => selectAgent(selAgent)} style={{ marginLeft: 'auto', padding: '3px 12px', borderRadius: 6, fontSize: 13, cursor: 'pointer', border: '1px solid #2a4a5a', background: 'transparent', color: '#9fb2c2' }}>✕ закрыть</button>
              </div>
              <div style={{ padding: '8px 12px' }}>
                <AgentPnlChart agentId={selAgent} />
                <AgentDrillDown detail={agentDetail} />
              </div>
            </div>
          </div>
        ), document.body)}

        {error && <div className="vc-error">Ошибка: {error}</div>}

        <div className="vc-table-wrap">
          <table className="vc-table">
            <thead>
              <tr>
                <th>time</th>
                <th>batch_id</th>
                <th>status</th>
                <th>residual_norm</th>
                <th>leg_count</th>
                <th>executed</th>
              </tr>
            </thead>
            <tbody>
              {items.length === 0 && (
                <tr>
                  <td colSpan={6} className="vc-empty">нет данных</td>
                </tr>
              )}
              {items.map((it, idx) => {
                const key = rowKey(it);
                const open = openKey === key;
                const d = detailByKey[key];
                return (
                  <React.Fragment key={`${it.batch_id}-${it.event_time_ms}`}>
                    <tr className={`vc-row ${open ? 'vc-row-open' : ''}`} onClick={() => toggleRow(it)}>
                      <td><span className="vc-caret">{open ? '▾' : '▸'}</span> {fmtTime(it.event_time_ms)}</td>
                      <td className="vc-mono">{it.batch_id}</td>
                      <td className={`vc-status s-${it.solver_status || 'unknown'}`}>{it.solver_status}</td>
                      <td className="vc-mono">{it.residual_norm}</td>
                      <td>{it.leg_count}</td>
                      <td className={it.executed ? 'vc-exec-yes' : 'vc-exec-no'}>{it.executed ? 'да' : 'нет'}</td>
                    </tr>
                    {open && (
                      <tr className="vc-detail-row">
                        <td colSpan={6}>
                          {!d && !detailErr && <div className="vc-detail-loading">загрузка детали…</div>}
                          {detailErr && <div className="vc-error">Ошибка: {detailErr}</div>}
                          {d && <ClearingDetail d={d} onChartPauseChange={setChartPaused} />}
                        </td>
                      </tr>
                    )}
                  </React.Fragment>
                );
              })}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  );
}

export default VectorClearingLive;
