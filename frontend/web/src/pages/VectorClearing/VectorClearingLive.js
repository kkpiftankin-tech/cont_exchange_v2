import React, { useEffect, useState, useCallback } from 'react';
import { createPortal } from 'react-dom';
import NavBar from '../../components/NavBar';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import './VectorClearingLive.css';

// F-05A: живой мониторинг векторного клиринга (ClickHouse vector_clearing_results).
// Ops-страница: market_data векторизует внешнюю ликвидность → matching решает
// QP Wx=0 (OSQP) + surplus → результат сюда.
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 3000;

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
function LiquidityChart({ venue, symbol, ts }) {
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
  const [data, setData] = useState(null);
  const [err, setErr] = useState('');
  const [loading, setLoading] = useState(false);

  useEffect(() => {
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
  }, [venue, symbol, ts, nBuy, nSell, anchorMode, theta]);

  if (err) return <div className="vc-error">Ошибка: {err}</div>;
  if (!data) return <div className="vc-note">{loading ? 'загрузка кривой…' : 'нет данных кривой'}</div>;

  // Готовые серии от backend (signed q, price). Тумблеры только скрывают/показывают.
  const vp = orient === 'vp';
  const rawBid = showRaw ? (data.rawBid || []) : [];
  const rawAsk = showRaw ? (data.rawAsk || []) : [];
  const vwapBid = showVwap ? (data.vwapBid || []) : [];
  const vwapAsk = showVwap ? (data.vwapAsk || []) : [];
  const safe = showSafe ? (data.safe || []) : [];   // safe-кривая в ОБЕИХ ориентациях
  const ceCurve = showCe ? (data.ceCurve || []) : []; // CE f(σ): плоская зона |σ−σ*|≤c
  const deadLow = Number(data.deadLow), deadHigh = Number(data.deadHigh);
  const deadZonePm = Number(data.deadZonePm);
  const fobBid = showFob ? (data.fobBid || []) : [];
  const fobAsk = showFob ? (data.fobAsk || []) : [];
  const anchor = Number(data.anchor);
  const bestBid = Number(data.bestBid), bestAsk = Number(data.bestAsk);
  const eng = data.engine || null;

  const all = [...rawBid, ...rawAsk, ...vwapBid, ...vwapAsk, ...safe, ...ceCurve, ...fobBid, ...fobAsk];
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

  const W = 620, H = 300, ml = 66, mr = 14, mt = 14, mb = 46; const pw = W - ml - mr, ph = H - mt - mb;
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

  return (
    <div className="vc-chart-wrap">
      <div className="vc-chart-ctrls" onClick={(e) => e.stopPropagation()}>
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
        <span className="vc-slope-badge">β_T = <b>{fmtSig(data.betaT)}</b> quote/base · α_T = {fmtSig(data.alphaT)} · α_ext = {fmtSig(data.alphaExt)} (bind {data.bindSide || '—'} L{data.bindLevel || '—'}){loading ? ' …' : ''}</span>
      </div>
      <svg width={W} height={H} className="vc-chart">
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
        {ceCurve.length > 0 && <polyline points={line(ceCurve)} fill="none" stroke="#e8c14a" strokeWidth="2.4" />}
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
        {ceCurve.length > 0 && <span className="vc-lg" style={{ color: '#e8c14a' }}>▨ CE зона комиссии/бездействия (|σ−σ*|≤c, c={fmtSig(deadZonePm)}‰)</span>}
        {(fobBid.length > 0 || fobAsk.length > 0) && <span className="vc-lg vc-lg-fob">- - FOB-кривая venue</span>}
        <span className="vc-lg vc-lg-anchor">- - anchor ({anchorMode}) {fmt(anchor)} · спред {Number(data.spreadBps).toFixed(2)} bps</span>
        {eng && <span className="vc-lg vc-lg-safe" title="то, что реально клирится (движок market_data)">движок: β_T={fmtSig(eng.betaT)} α_T={fmtSig(eng.alphaT)} [{eng.model}]</span>}
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
function AgentPositionsTables({ agentRows, title, onSelectAgent, selectedId }) {
  const rows = Array.isArray(agentRows) ? agentRows : [];
  const clickable = typeof onSelectAgent === 'function';
  const rowProps = (a) => clickable ? {
    onClick: () => onSelectAgent(a.agent_id),
    style: { cursor: 'pointer', background: a.agent_id === selectedId ? 'rgba(37,99,235,0.12)' : undefined },
    title: 'клик — разбор агента (цена/порог/хедж/исполнение)'
  } : {};
  const sgn = (n) => (Number(n) > 1e-9 ? '+' : '') + fmtNum(n);
  const leg = (v, cur) => (v == null) ? <span title="ещё нет цены пары в этом такте">—</span> : (
    <span className={Number(v) > 1e-9 ? 'vc-side-ask' : Number(v) < -1e-9 ? 'vc-side-bid' : ''}>
      {sgn(v)}<span style={{ opacity: 0.6, fontSize: '0.85em' }}> {cur}</span>
    </span>
  );
  const dash = <span title="ещё нет цены пары для перевода стоимости в объём" style={{ opacity: 0.4 }}>—</span>;
  const zero = <span title="агент не двигался в этом такте (Δ=0)" style={{ opacity: 0.3 }}>0</span>;
  const tRows = rows.filter((a) => a.agent_kind === 'translator');
  const aRows = rows.filter((a) => a.agent_kind === 'arbitrageur');
  return (
    <div className="vc-sec">
      <div className="vc-sec-title">{title}</div>
      <div className="vc-sec-body">
        <div className="vc-sub-head">Переводчики (пара base/quote) — <b>{tRows.length}</b></div>
        {tRows.length > 0 ? (
          <table className="vc-sub-table">
            <thead><tr>
              <th>agent_id</th><th>пара</th><th>площадка</th>
              <th title="Δ ЭТОГО такта в БАЗОВОЙ валюте">Δ база</th>
              <th title="Δ ЭТОГО такта в КОТИРУЕМОЙ валюте">Δ котир.</th>
              <th title="ТЕКУЩАЯ позиция в БАЗОВОЙ валюте">позиция база</th>
              <th title="ТЕКУЩАЯ позиция в КОТИРУЕМОЙ валюте">позиция котир.</th>
            </tr></thead>
            <tbody>
              {tRows.map((a, i) => (
                <tr key={(a.agent_id || '') + '|t|' + i} {...rowProps(a)}>
                  <td className="vc-mono">{a.agent_id}</td>
                  <td className="vc-mono">{a.pair}</td>
                  <td>{a.venue || '—'}</td>
                  <td className="vc-mono">{a.hasDelta ? leg(a.delta_base, a.base) : zero}</td>
                  <td className="vc-mono">{a.hasDelta ? (a.quote ? leg(a.delta_quote, a.quote) : zero) : zero}</td>
                  <td className="vc-mono"><b>{leg(a.pos_base, a.base)}</b></td>
                  <td className="vc-mono">{a.quote ? <b>{leg(a.pos_quote, a.quote)}</b> : dash}</td>
                </tr>
              ))}
            </tbody>
          </table>
        ) : <div className="vc-note vc-note-tight">нет переводчиков с позицией</div>}

        <div className="vc-sub-head" style={{ marginTop: 14 }}>Арбитражёры (перевоз актива между площадками) — <b>{aRows.length}</b></div>
        {aRows.length > 0 ? (
          <table className="vc-sub-table">
            <thead><tr>
              <th>agent_id</th><th>актив</th><th>маршрут</th>
              <th title="Δ ЭТОГО такта на площадке-источнике">Δ источник</th>
              <th title="Δ ЭТОГО такта на площадке-приёмнике">Δ приёмник</th>
              <th title="ТЕКУЩАЯ позиция на площадке-источнике">позиция источник</th>
              <th title="ТЕКУЩАЯ позиция на площадке-приёмнике">позиция приёмник</th>
            </tr></thead>
            <tbody>
              {aRows.map((a, i) => {
                const r = a.arb || {};
                const lblS = `${a.base}@${r.src || '?'}`;
                const lblD = `${a.base}@${r.dst || '?'}`;
                return (
                  <tr key={(a.agent_id || '') + '|a|' + i} {...rowProps(a)}>
                    <td className="vc-mono">{a.agent_id}</td>
                    <td className="vc-mono">{a.base}</td>
                    <td className="vc-mono">{(r.src || '?')} ↔ {(r.dst || '?')}</td>
                    <td className="vc-mono">{a.hasDelta ? leg(r.dleg_src, lblS) : zero}</td>
                    <td className="vc-mono">{a.hasDelta ? leg(r.dleg_dst, lblD) : zero}</td>
                    <td className="vc-mono"><b>{leg(r.leg_src, lblS)}</b></td>
                    <td className="vc-mono"><b>{leg(r.leg_dst, lblD)}</b></td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        ) : <div className="vc-note vc-note-tight">нет арбитражёров с позицией</div>}

        <div className="vc-note vc-note-tight">
          <b>Переводчик</b> держит пару <b>base/quote</b>: одна нога +, другая − (при SELL base он short базовую / long котируемую).
          <b> Арбитражёр</b> перевозит один актив между двумя площадками: <b>short на источнике / long на приёмнике</b> (напр. −BTC@binance / +BTC@okx). «v3» в id — это площадка <code>uniswap_v3</code>, не версия.
          <b> Прочерк</b>: в колонке Δ — агент не двигался в этом такте (позиция при этом показана); в позиции — ещё нет цены пары. Ноги считает BFF из <code>c_j</code> и цены пары (money-path, ADR-057).
        </div>
      </div>
    </div>
  );
}

// Drill-down по выбранному агенту: позиция → сдвиг цены от позиции (skew) → порог q
// → пробой → хедж-заявка в паре → публичные сделки → имитируемое (полное/частичное)
// исполнение. Данные из /agent-detail (BFF), здесь только рендер.
function AgentDrillDown({ detail }) {
  if (!detail) return <div className="vc-note vc-note-tight">кликните агента в таблице выше — покажу цену/порог/хедж/исполнение.</div>;
  const a = detail.agent || {};
  const sk = detail.skew || {};
  const b = detail.band || {};
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
        <div style={{ fontSize: 13 }}>anchor_eff = anchor − clamp(γ·c_j), γ={sk.gamma}, клэмп={sk.clamp}‰</div>
        <div>текущий сдвиг цены:{' '}
          <b style={{ color: psColor }}>{ps >= 0 ? '+' : ''}{ps.toFixed(3)} ‰</b>{' '}
          ({c > 1e-9 ? 'позиция + ⇒ цена ↓ (возврат к нулю)' : c < -1e-9 ? 'позиция − ⇒ цена ↑ (возврат к нулю)' : 'позиция 0 ⇒ сдвига нет'})
        </div>
      </div>
      <div style={box}>
        <div style={head}>Порог по позиции (band q)</div>
        {isT ? (
          <div>|c_j| = <b>{kusd(b.absC)}</b> vs порог q = <b>{kusd(b.q)}</b> ⇒ избыток = <b>{kusd(b.excess)}</b>{'  '}
            <span style={{ padding: '1px 8px', borderRadius: 10, fontWeight: 600, color: b.breached ? '#f0b0b0' : '#8fe0b0', background: b.breached ? '#5a2020' : '#1f3a2a', border: `1px solid ${b.breached ? '#7a3030' : '#2a4a3a'}` }}>
              {b.breached ? 'ПОРОГ ПРЕВЫШЕН' : 'в полосе'}</span>
          </div>
        ) : (b.q != null ? (
          <div>|c_j| = <b>{kusd(b.absC)}</b> vs порог q = <b>{kusd(b.q)}</b> ⇒ избыток = <b>{kusd(b.excess)}</b>{'  '}
            <span style={{ padding: '1px 8px', borderRadius: 10, fontWeight: 600, color: b.breached ? '#f0b0b0' : '#8fe0b0', background: b.breached ? '#5a2020' : '#1f3a2a', border: `1px solid ${b.breached ? '#7a3030' : '#2a4a3a'}` }}>
              {b.breached ? 'ПОРОГ ПРЕВЫШЕН' : 'в полосе'}</span>
            <div style={{ fontSize: 12, color: MUTE, marginTop: 2 }}>порог арбитражёра шире: round-trip 2 биржи (комиссия ×2)</div>
          </div>
        ) : <div style={{ fontSize: 13 }}>порог band = k_band·φ_rt (комиссия внешних бирж).</div>)}
      </div>
      {isT && b.breached && (
        <div style={box}>
          <div style={head}>Хедж-заявка → публичные сделки → имитируемое исполнение</div>
          {h ? (
            <>
              <div>Заявка (пара): <b>{h.pair}</b> <b>{h.side}</b> target <b>{num(h.targetQty)}</b> @ лимит {num(h.limitPrice, 2)}</div>
              <div style={{ marginTop: 4 }}>Ближайшие по цене публичные сделки вокруг лимита (контекст последних 60, объём пересекающих по цене = <b>{num(h.crossVol)}</b>). «Исполнила бы» — по цене; фактический матч идёт по свежему окну (см. причину ниже).</div>
              {(() => {
                const L = Number(h.limitPrice) || 0;
                const isSell = h.side === 'SELL';
                const ts = (h.considered || []).map((t) => ({ price: Number(t.price), qty: Number(t.qty), age_ms: t.age_ms }));
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
                    <td className="vc-mono">{t.age_ms}</td>
                    <td style={{ color: fills ? GREEN : MUTE }}>{fills ? '✓ исполнила бы' : '—'}</td>
                  </tr>
                );
                return (
                  <table className="vc-sub-table" style={{ marginTop: 4 }}>
                    <thead><tr><th>цена</th><th>объём (бар)</th><th>возраст, мс</th><th>vs лимит</th></tr></thead>
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
function ClearingDetail({ d }) {
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
                            <LiquidityChart venue={s.exchange} symbol={s.instrument} ts={d.event_time_ms} />
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
      <AgentPositionsTables agentRows={agentRows} title="4. Позиции агентов по парам валют (money-path)" />
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
  const [liveAt, setLiveAt] = useState(null);        // время последнего обновления живой панели
  const [selAgent, setSelAgent] = useState(null);    // выбранный агент для drill-down
  const [agentDetail, setAgentDetail] = useState(null); // /agent-detail выбранного агента
  const [openKey, setOpenKey] = useState(null);
  const [detailByKey, setDetailByKey] = useState({});
  const [detailErr, setDetailErr] = useState('');
  // Runtime-конфиг цикла батч-клиринга (окно/staleness).
  const [cfgWindowMs, setCfgWindowMs] = useState('');
  const [cfgStaleMs, setCfgStaleMs] = useState('');
  const [cfgFeeBps, setCfgFeeBps] = useState('');   // комиссия тейкера, bps (0 = линейно)
  const [cfgSkewGamma, setCfgSkewGamma] = useState('');   // inventory-skew γ (‰/k-USDT, 0=выкл)
  const [cfgSkewMaxPm, setCfgSkewMaxPm] = useState('');   // клэмп смещения (‰)
  const [cfgMsg, setCfgMsg] = useState('');
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
    } catch (e) { /* PG может быть недоступен — оставляем пустым */ }
  }, []);

  const saveConfig = useCallback(async () => {
    setCfgSaving(true); setCfgMsg('');
    try {
      const r = await axios.post(`${API_BASE}/vector-clearing/config`, {
        batch_window_ms: Number(cfgWindowMs), stale_level_ms: Number(cfgStaleMs),
        ce_taker_fee_bps: Number(cfgFeeBps),
        ce_inv_skew_gamma: Number(cfgSkewGamma), ce_inv_skew_max_pm: Number(cfgSkewMaxPm)
      }, { timeout: 8000 });
      setCfgWindowMs(String(r.data.batch_window_ms));
      setCfgStaleMs(String(r.data.stale_level_ms));
      setCfgFeeBps(String(r.data.ce_taker_fee_bps));
      setCfgSkewGamma(String(r.data.ce_inv_skew_gamma));
      setCfgSkewMaxPm(String(r.data.ce_inv_skew_max_pm));
      setCfgMsg('применено ✓ (matching/market_data подхватят ≤2с)');
    } catch (e) {
      setCfgMsg('ошибка: ' + (e.message || 'не сохранено'));
    } finally { setCfgSaving(false); }
  }, [cfgWindowMs, cfgStaleMs, cfgFeeBps, cfgSkewGamma, cfgSkewMaxPm]);

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
      setLiveRows(all.filter((a) => Math.abs(Number(a.c_total != null ? a.c_total : a.c_position)) > 1e-9 || a.hasDelta));
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
    if (isAuth) { load(); loadLiveness(); }
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
          <button className="vc-config-apply" onClick={saveConfig} disabled={cfgSaving}>
            {cfgSaving ? '…' : 'Применить'}
          </button>
          {cfgMsg && <span className="vc-config-msg">{cfgMsg}</span>}
          <span className="vc-config-hint">комиссия 0 = линейные кривые; skew γ&gt;0 = обратная связь позиция→цена (позиции не дрейфуют), γ=0 = выкл. Крутить γ/клэмп вживую и смотреть позиции.</span>
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
              : <AgentPositionsTables agentRows={liveRows}
                  title="Позиции агентов — ЖИВОЙ просмотр (money-path) · клик по строке — разбор агента"
                  onSelectAgent={selectAgent} selectedId={selAgent} />}
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
                          {d && <ClearingDetail d={d} />}
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
