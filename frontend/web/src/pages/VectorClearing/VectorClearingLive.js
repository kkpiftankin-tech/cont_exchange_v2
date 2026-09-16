import React, { useEffect, useState, useCallback } from 'react';
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
// (3) черновики хедж-заявок.
function ClearingDetail({ d }) {
  const src = Array.isArray(d.source) ? d.source : [];
  const prices = Array.isArray(d.clearingPrices) ? d.clearingPrices : [];
  const rates = Array.isArray(d.clearingRates) ? d.clearingRates : [];
  const drafts = Array.isArray(d.hedgeDrafts) ? d.hedgeDrafts : [];
  const ce = d.cePosition && Array.isArray(d.cePosition.assets) ? d.cePosition.assets : [];
  // F-18 v2 (Стадия 1 наблюдаемости, ADR-061/063): позиции CE-агентов
  // (переводчики/арбитражёры) — из ledger.GetAgentPositions (реальный DTO,
  // без фейков). Группировка: сначала переводчики, затем арбитражёры,
  // затем прочее; внутри группы — по agent_id/активу.
  const agentPositions = Array.isArray(d.agentPositions) ? d.agentPositions : [];
  const agentKindOrder = { translator: 0, arbitrageur: 1 };
  const agentKindLabel = (k) => (k === 'translator' ? 'переводчик' : k === 'arbitrageur' ? 'арбитражёр' : (k || '—'));
  const sortedAgentPositions = [...agentPositions].sort((a, b) => {
    const ka = agentKindOrder[a.agent_kind] != null ? agentKindOrder[a.agent_kind] : 2;
    const kb = agentKindOrder[b.agent_kind] != null ? agentKindOrder[b.agent_kind] : 2;
    if (ka !== kb) return ka - kb;
    if (a.agent_id !== b.agent_id) return String(a.agent_id).localeCompare(String(b.agent_id));
    return String(a.asset).localeCompare(String(b.asset));
  });
  const twoSided = src.some((s) => s.twoSided);
  const sgn = (n) => (Number(n) > 1e-9 ? '+' : '') + fmtNum(n);
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

      {/* (3) Позиция биржи CE по этому клирингу: ДО → Δ → ПОСЛЕ + пороги + хедж (F-18) */}
      <div className="vc-sec">
        <div className="vc-sec-title">
          3. Позиция биржи CE — ДО → Δ клиринга → ПОСЛЕ{d.cePosition && d.cePosition.armedCount > 0 ? <> · <b>{d.cePosition.armedCount}</b> хедж</> : ''}
        </div>
        <div className="vc-sec-body">
          {ce.length > 0 ? (
            <>
            <table className="vc-sub-table">
              <thead>
                <tr><th>актив</th><th>позиция ДО</th><th>Δ клиринга</th><th title="ПОСЛЕ = ДО + Δ">позиция ПОСЛЕ</th><th title="CE_HEDGE_THRESHOLD_&lt;ASSET&gt;">θ порог</th><th>хедж → внешняя биржа</th></tr>
              </thead>
              <tbody>
                {ce.map((r, i) => (
                  <tr key={r.asset || i} className={r.hedgeArmed ? 'vc-row-armed' : ''}>
                    <td>{r.asset}</td>
                    <td className="vc-mono">{sgn(r.before)}</td>
                    <td className={`vc-mono ${Number(r.delta) > 0 ? 'vc-side-ask' : Number(r.delta) < 0 ? 'vc-side-bid' : ''}`}>{sgn(r.delta)}</td>
                    <td className={`vc-mono ${Number(r.after) > 0 ? 'vc-side-ask' : Number(r.after) < 0 ? 'vc-side-bid' : ''}`}><b>{sgn(r.after)}</b></td>
                    <td className="vc-mono">{r.threshold == null ? '∞' : fmtNum(r.threshold)}</td>
                    <td>
                      {r.hedge
                        ? <span className={r.hedge.side === 'SELL' ? 'vc-side-bid' : 'vc-side-ask'}><b>{r.hedge.side}</b> {fmtNum(r.hedge.qty)} {r.hedge.instrument}</span>
                        : <span className="vc-empty">—</span>}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
            <div className="vc-note vc-note-tight">
              F-18 (ADR-054): позиция биржи = проекция вектора клиринга x на активы (двойная запись).
              ПОСЛЕ = ДО + Δ; хедж на внешнюю биржу эмиттится при <b>|ПОСЛЕ| &gt; θ</b> (short→BUY, long→SELL).
              Standalone по батчу (ДО=0); накопление и полное состояние — во вкладке <b>Treasury</b> и с движком ce-treasury (Phase 2).
            </div>
            </>
          ) : (
            <div className="vc-note">
              Позиция биржи не выведена: нет клиринговых цен/x для этого батча, либо активы без порога θ.
              Настройка порогов — <span className="vc-mono">CE_HEDGE_THRESHOLD_&lt;ASSET&gt;</span>.
            </div>
          )}
        </div>
      </div>

      {/* (4) Черновики заявок на хеджирование */}
      <div className="vc-sec">
        <div className="vc-sec-title">4. Черновики хедж-заявок на внешние биржи — <b>{drafts.length}</b></div>
        <div className="vc-sec-body">
          {drafts.length > 0 ? (
            <table className="vc-sub-table">
              <thead>
                <tr><th>биржа</th><th>инструмент</th><th>сторона</th><th>объём (target_qty)</th><th>лимит-цена</th><th>intent_id</th></tr>
              </thead>
              <tbody>
                {drafts.map((h, i) => (
                  <tr key={h.intent_id || i}>
                    <td>{h.venue}</td>
                    <td>{h.instrument}</td>
                    <td className={h.side === 'SELL' ? 'vc-side-bid' : 'vc-side-ask'}>{h.side}</td>
                    <td className="vc-mono">{h.target_qty}</td>
                    <td className="vc-mono">{h.limit_price}</td>
                    <td className="vc-mono vc-ellipsis" title={h.intent_id}>{h.intent_id}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          ) : (
            <div className="vc-note">
              Нет исполняемого объёма (все x ≈ 0 — нет арбитража), поэтому хедж-черновики не формируются.
              Черновик появится для сегментов с x&gt;0 (bid→SELL, ask→BUY, лимит = effective_price).
            </div>
          )}
        </div>
      </div>

      {/* (5) Позиции агентов (переводчики/арбитражёры) — F-18 v2 Стадия 1
          наблюдаемости: реальный DTO ledger.GetAgentPositions, без фейков. */}
      <div className="vc-sec">
        <div className="vc-sec-title">5. Позиции агентов (переводчики / арбитражёры) — <b>{agentPositions.length}</b></div>
        <div className="vc-sec-body">
          {sortedAgentPositions.length > 0 ? (
            <>
            <table className="vc-sub-table">
              <thead>
                <tr>
                  <th>agent_id</th>
                  <th>тип</th>
                  <th>актив</th>
                  <th>площадка</th>
                  <th title="c_j, ЗНАКОВАЯ: + = длинная (купил/накопил), − = короткая (продал/должен)">позиция (знак.)</th>
                  <th title="committed (Э3+, полоса ±q); пока всегда 0">in_flight</th>
                </tr>
              </thead>
              <tbody>
                {sortedAgentPositions.map((p, i) => (
                  <tr key={(p.agent_id || '') + '|' + (p.asset || '') + '|' + (p.venue || '') + '|' + i}>
                    <td className="vc-mono">{p.agent_id}</td>
                    <td>{agentKindLabel(p.agent_kind)}</td>
                    <td>{p.asset}</td>
                    <td>{p.venue || '—'}</td>
                    <td className={`vc-mono ${Number(p.position) > 0 ? 'vc-side-ask' : Number(p.position) < 0 ? 'vc-side-bid' : ''}`}>
                      <b>{sgn(p.position)}</b>
                    </td>
                    <td className="vc-mono">{sgn(p.in_flight)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            <div className="vc-note vc-note-tight">
              Знак позиции = направление: <b>+</b> длинная (агент купил/накопил актив), <b>−</b> короткая
              (агент продал/должен актив). Это <b>ТЕКУЩАЯ накопленная позиция агента</b> (ledger.GetAgentPositions),
              а НЕ снимок именно этого клиринга — привязку к конкретному batch_id добавит Стадия 3
              (execution.intents/ExecutionReport с agent_id).
            </div>
            </>
          ) : (
            <div className="vc-note">нет данных по агентам (включите CE_AGENT_POS)</div>
          )}
        </div>
      </div>
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
  const [openKey, setOpenKey] = useState(null);
  const [detailByKey, setDetailByKey] = useState({});
  const [detailErr, setDetailErr] = useState('');
  // Runtime-конфиг цикла батч-клиринга (окно/staleness).
  const [cfgWindowMs, setCfgWindowMs] = useState('');
  const [cfgStaleMs, setCfgStaleMs] = useState('');
  const [cfgFeeBps, setCfgFeeBps] = useState('');   // комиссия тейкера, bps (0 = линейно)
  const [cfgMsg, setCfgMsg] = useState('');
  const [cfgSaving, setCfgSaving] = useState(false);

  const loadConfig = useCallback(async () => {
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/config`, { timeout: 8000 });
      setCfgWindowMs(String(r.data.batch_window_ms ?? 1000));
      setCfgStaleMs(String(r.data.stale_level_ms ?? 60000));
      setCfgFeeBps(String(r.data.ce_taker_fee_bps ?? -1));
    } catch (e) { /* PG может быть недоступен — оставляем пустым */ }
  }, []);

  const saveConfig = useCallback(async () => {
    setCfgSaving(true); setCfgMsg('');
    try {
      const r = await axios.post(`${API_BASE}/vector-clearing/config`, {
        batch_window_ms: Number(cfgWindowMs), stale_level_ms: Number(cfgStaleMs),
        ce_taker_fee_bps: Number(cfgFeeBps)
      }, { timeout: 8000 });
      setCfgWindowMs(String(r.data.batch_window_ms));
      setCfgStaleMs(String(r.data.stale_level_ms));
      setCfgFeeBps(String(r.data.ce_taker_fee_bps));
      setCfgMsg('применено ✓ (market_data подхватит ≤2с)');
    } catch (e) {
      setCfgMsg('ошибка: ' + (e.message || 'не сохранено'));
    } finally { setCfgSaving(false); }
  }, [cfgWindowMs, cfgStaleMs, cfgFeeBps]);

  const rowKey = (it) => `${it.batch_id}|${it.event_time_ms}`;

  const toggleRow = useCallback(async (it) => {
    const key = rowKey(it);
    if (openKey === key) { setOpenKey(null); return; }
    setOpenKey(key);
    setDetailErr('');
    if (!detailByKey[key]) {
      try {
        const resp = await axios.get(`${API_BASE}/vector-clearing/detail`, {
          params: { batch_id: it.batch_id, ts: it.event_time_ms }, timeout: 12000
        });
        setDetailByKey((m) => ({ ...m, [key]: resp.data }));
      } catch (e) {
        setDetailErr(e.message || 'ошибка загрузки детали');
      }
    }
  }, [openKey, detailByKey]);

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
      setError('');
    } catch (e) {
      setError(e.message || 'ошибка загрузки');
    }
  }, []);

  useEffect(() => {
    if (isAuth) { load(); loadConfig(); }
  }, [isAuth, load, loadConfig]);

  useInterval(() => {
    if (isAuth) load();
  }, POLL_INTERVAL_MS);

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  return (
    <div className="vc-page">
      <NavBar />
      <div className="vc-content">
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
          <button className="vc-config-apply" onClick={saveConfig} disabled={cfgSaving}>
            {cfgSaving ? '…' : 'Применить'}
          </button>
          {cfgMsg && <span className="vc-config-msg">{cfgMsg}</span>}
          <span className="vc-config-hint">комиссия 0 = линейные кривые (нет зоны бездействия); больше окно / staleness → больше кривых накапливается перед клирингом</span>
        </div>

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
