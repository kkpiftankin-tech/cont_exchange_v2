import React, { useEffect, useMemo, useState, useCallback, useRef } from 'react';
import axios from 'axios';
import useInterval from '../hooks/useInterval';
import './RawOrderBook.css';

// Сырой стакан биржи (venue_snapshots): график глубины (цена × накопленный объём). Только отображение — расчётов домена нет.
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const REFRESH_MS = 1000;
// Цвета бирж для кривых «по всем биржам» (стабильно по id; неизвестные — серый).
const VENUE_COLORS = { binance: '#f3ba2f', okx: '#7aa2ff', kraken: '#b08cff', coinbase: '#4aa3ff', uniswap_v3: '#ff6ec7' };
const venueColor = (id) => VENUE_COLORS[id] || '#9aa7b4';
const LIVE_HEARTBEAT_MS = 2500;   // как часто панель подтверждает аренду глубокого опроса
const LIVE_LEASE_MS = 6000;       // короткая аренда: смена пары освобождает место за ≤6 с, без сигналов опрос гаснет сам
const CURVE_REFRESH_MS = 3000;  // глубокий стакан обновляется ~раз в 3 с — пересчёт кривых по нему
// Способы снятия наклона §6.2 (как на вкладке Clearing): номер, подпись, цвет.
const METHODS = [
  { id: 1, label: 'M1 касательная', color: '#ff9f40' },
  { id: 2, label: 'M2 МНК', color: '#40d0ff' },
  { id: 3, label: 'M3 по выгоде', color: '#b980ff' },
  { id: 4, label: 'M4 минорант', color: '#70d080' },
];
const DEPTH = 1000;      // максимум уровней с BFF (сколько есть в снимке)

const fmtPrice = (p) => (p >= 100 ? p.toFixed(2) : p >= 1 ? p.toFixed(4) : p.toFixed(6));
const fmtQty = (q) => (q >= 100 ? q.toFixed(2) : q.toFixed(5));
// Объём со знаком, как в системе: покупка (ask) +, продажа (bid) −.
const fmtSig = (v) => (Number.isFinite(Number(v)) ? Number(v).toPrecision(4) : '—');
const fmtSigned = (q) => (q < 0 ? `−${fmtQty(-q)}` : q > 0 ? `+${fmtQty(q)}` : '0');



// График глубины: X — цена, Y — накопленный объём (base). Bids слева, asks справа.
// zoom — во сколько раз сужаем окно по цене вокруг mid (только отображение).
const W = 760, H = 420, ML = 8, MR = 62, MT = 12, MB = 26;
const ZOOM_MAX = 20000;
// Чувствительность колеса: щелчок мыши (deltaY≈100) ≈ ×1.5, трекпад — плавно.
const ZOOM_SENS = 0.004;
const ticks = (lo, hi, n) => {
  const out = [];
  for (let i = 0; i <= n; i++) out.push(lo + ((hi - lo) * i) / n);
  return out;
};
const AUTO_HALF_PCT = 0.003;  // окно по умолчанию: ±0.3% от mid (или весь стакан, если он уже)
const DepthChart = ({ bids, asks, zoom, onZoom, slope, curveOn, now, agg, aggOn, aggVenue }) => {
  const svgRef = useRef(null);
  const effZoomRef = useRef(1);  // эффективный масштаб текущего кадра (zoom=null ⇒ авто)
  // Колесо мыши — масштаб по цене (как на Binance); non-passive, чтобы не скроллить страницу.
  useEffect(() => {
    const el = svgRef.current;
    if (!el) return undefined;
    const onWheel = (e) => {
      e.preventDefault();
      const f = Math.exp(-Math.max(-300, Math.min(300, e.deltaY)) * ZOOM_SENS);
      onZoom(Math.min(ZOOM_MAX, Math.max(1, effZoomRef.current * f)));
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  });
  if (!bids.length && !asks.length) return null;
  const bestBid = bids.length ? bids[0].p : null;
  const bestAsk = asks.length ? asks[0].p : null;
  const mid = bestBid != null && bestAsk != null ? (bestBid + bestAsk) / 2 : (bestBid ?? bestAsk);
  const spread = bestBid != null && bestAsk != null ? bestAsk - bestBid : 0;
  const all = [...bids, ...asks];
  const fullHalf = Math.max(mid - Math.min(...all.map((l) => l.p)), Math.max(...all.map((l) => l.p)) - mid);
  // Окно по умолчанию (zoom=null): ±0.3% mid, но не шире самого стакана. Иначе один далёкий
  // уровень (напр. выброс Coinbase) растягивает ось, и вблизи спреда ничего не видно.
  const autoHalf = Math.min(fullHalf, mid * AUTO_HALF_PCT);
  const effZoom = zoom == null ? Math.max(1, fullHalf / Math.max(autoHalf, 1e-12)) : zoom;
  effZoomRef.current = effZoom;
  const half = Math.max(fullHalf / effZoom, spread * 2, mid * 1e-7);
  const pMin = mid - half, pMax = mid + half;
  const vis = (rows) => rows.filter((l) => l.p >= pMin && l.p <= pMax);
  const vb = vis(bids), va = vis(asks);
  const vMax = Math.max(vb.length ? vb[vb.length - 1].cum : 0, va.length ? va[va.length - 1].cum : 0) || 1;
  const x = (p) => ML + ((p - pMin) / (pMax - pMin)) * (W - ML - MR);
  // Y — объём со знаком: asks вверх (+), bids перевёрнуты вниз (−); шкала общая [−vMax, +vMax].
  const y = (v) => MT + ((vMax - v) / (2 * vMax)) * (H - MT - MB);
  // Ступенчатая область от лучшей цены вглубь стакана до края окна.
  const area = (rows, edge, sign) => {
    if (!rows.length) return '';
    let d = `M${x(rows[0].p)},${y(0)} L${x(rows[0].p)},${y(sign * rows[0].cum)}`;
    for (let i = 1; i < rows.length; i++) d += ` L${x(rows[i].p)},${y(sign * rows[i - 1].cum)} L${x(rows[i].p)},${y(sign * rows[i].cum)}`;
    const last = rows[rows.length - 1];
    return d + ` L${x(edge)},${y(sign * last.cum)} L${x(edge)},${y(0)} Z`;
  };
  // Кривые ликвидности: прямая P(q)=anchor+β·q на каждый выбранный способ M1–M4 (расчёт
  // venues по глубокому стакану, §6.2). Одна кривая на обе стороны; рисуется ВО ВЕСЬ график:
  // концы прямой — на левом/правом краю графика (y = (цена − anchor)/β, q со знаком).
  const lines = (slope ? slope.methods : [])
    .filter((m) => curveOn[m.method] && m.betaT > 0)
    .map((m) => ({ ...m, meta: METHODS.find((k) => k.id === m.method) }));
  const linePts = (m) => `${x(pMin)},${y((pMin - slope.anchor) / m.betaT)} ${x(pMax)},${y((pMax - slope.anchor) / m.betaT)}`;
  // Кривые «по всем биржам» (расчёт BFF /venues/slope-curves-all): прямые q = k·p + c в осях
  // «цена → объём со знаком». Фронт только вычисляет концы прямой на краях графика.
  const aggLines = [];
  if (agg) {
    if (aggOn.venues) {
      agg.venues.forEach((v) => aggLines.push({ key: 'v' + v.venue, k: v.k, c: v.c, color: venueColor(v.venue), width: v.venue === aggVenue ? 2.6 : 1.6, dash: null, label: v.venue }));
    }
    if (aggOn.all && agg.all.count > 0) aggLines.push({ key: 'all', k: agg.all.k, c: agg.all.c, color: '#ffffff', width: 2.8, dash: null, label: `Σ все биржи (${agg.all.count})` });
    if (aggOn.ex && agg.ex.count > 0) aggLines.push({ key: 'ex', k: agg.ex.k, c: agg.ex.c, color: '#ff6ec7', width: 2.8, dash: '7 4', label: `Σ все кроме ${aggVenue || 'выбранной'} (${agg.ex.count})` });
  }
  const aggPts = (l) => `${x(pMin)},${y(l.k * pMin + l.c)} ${x(pMax)},${y(l.k * pMax + l.c)}`;
  const bps = mid > 0 ? (spread / mid) * 1e4 : 0;
  return (
    <>
      <div className="rob-quote">
        <span className="rob-q-bid">Покупка (bid) {bestBid != null ? fmtPrice(bestBid) : '—'}</span>
        <span className="rob-q-spread">спред {fmtPrice(spread)} ({bps.toFixed(2)} б.п.)</span>
        <span className="rob-q-ask">Продажа (ask) {bestAsk != null ? fmtPrice(bestAsk) : '—'}</span>
      </div>
      <div className="rob-range">
        <span className="rob-lg rob-lg-vwap-bid">▬ VWAP продажи (bid)</span>
        <span className="rob-lg rob-lg-vwap-ask">▬ VWAP покупки (ask)</span>
        {' · '}колесо мыши — масштаб · окно ±{fmtPrice(half)} ({((half / mid) * 100).toFixed(3)}%) · уровней в окне: {vb.length} bid / {va.length} ask
      </div>
      {lines.length > 0 && (
        <div className="rob-range">
          {lines.map((m) => (
            <span key={m.method} className="rob-lg" style={{ color: m.meta.color }}
              title={m.method === 4 ? 'минорант: §6.3-safe, не обещает больше глубины, чем есть' : 'M1–M3 могут локально переобещать глубину (§6.3)'}>
              ▬ {m.meta.label} β={fmtSig(m.betaT)}{slope.selectedMethod === m.method ? ' ★клиринг' : ''}
            </span>
          ))}
          {' · '}P(q)=anchor+β·q, anchor={fmtPrice(slope.anchor)} · расчёт по {slope.levels_bid}/{slope.levels_ask} ур.,
          снимок {Math.max(0, Math.round((now - Number(slope.event_time_ms)) / 1000))} с назад
        </div>
      )}
      {aggLines.length > 0 && (
        <div className="rob-range">
          {aggLines.map((l) => (
            <span key={l.key} className="rob-lg" style={{ color: l.color }}>{l.dash ? '┅' : '▬'} {l.label}</span>
          ))}
          {' · '}способ M{agg.method}{agg.selectedMethod === agg.method ? ' ★клиринг' : ''}: учтено {agg.venues.length} из {agg.requested} бирж
          {agg.missing.length > 0 && (
            <span className="rob-err"> · нет глубокого стакана: {agg.missing.map((m) => m.venue).join(', ')}</span>
          )}
        </div>
      )}
      <svg ref={svgRef} className="rob-chart" viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="xMidYMid meet">
        {ticks(-vMax, vMax, 4).map((v) => (
          <g key={`y${v}`}>
            <line x1={ML} x2={W - MR} y1={y(v)} y2={y(v)} className="rob-grid" />
            <text x={W - MR + 4} y={y(v) + 3} className="rob-axis">{fmtSigned(v)}</text>
          </g>
        ))}
        {ticks(pMin, pMax, 4).map((p) => (
          <text key={`x${p}`} x={x(p)} y={H - 8} className="rob-axis" textAnchor="middle">{fmtPrice(p)}</text>
        ))}
        {bestBid != null && bestAsk != null && (
          <rect x={x(bestBid)} y={MT} width={Math.max(x(bestAsk) - x(bestBid), 1.5)} height={H - MT - MB} className="rob-spread-band" />
        )}
        <path d={area(vb, pMin, -1)} className="rob-area-bid" />
        <path d={area(va, pMax, 1)} className="rob-area-ask" />
        <line x1={ML} x2={W - MR} y1={y(0)} y2={y(0)} className="rob-zero" />
        {/* Кривая VWAP: цена исполнения объёма Σ (x — VWAP уровня, y — накопленный объём) */}
        <polyline points={vb.map((l) => `${x(l.vwap)},${y(-l.cum)}`).join(' ')} className="rob-vwap-bid" />
        <polyline points={va.map((l) => `${x(l.vwap)},${y(l.cum)}`).join(' ')} className="rob-vwap-ask" />
        <line x1={x(mid)} x2={x(mid)} y1={MT} y2={H - MB} className="rob-mid-line" />
        <defs>
          <clipPath id="rob-clip"><rect x={ML} y={MT} width={W - ML - MR} height={H - MT - MB} /></clipPath>
        </defs>
        <g clipPath="url(#rob-clip)">
          {aggLines.map((l) => <polyline key={l.key} points={aggPts(l)} className="rob-slope" style={{ stroke: l.color, strokeWidth: l.width, strokeDasharray: l.dash || undefined }} />)}
          {lines.map((m) => <polyline key={m.method} points={linePts(m)} className="rob-slope" style={{ stroke: m.meta.color }} />)}
        </g>
        <text x={W - MR + 4} y={MT - 2} className="rob-axis rob-axis-t">Σ объём</text>
        <text x={ML + 4} y={y(0) - 4} className="rob-axis rob-axis-t">покупка (ask) +</text>
        <text x={ML + 4} y={y(0) + 12} className="rob-axis rob-axis-t">продажа (bid) −</text>
      </svg>
    </>
  );
};

const RawOrderBook = () => {
  const [venues, setVenues] = useState([]);
  const [venue, setVenue] = useState('');
  const [symbol, setSymbol] = useState('');
  const [book, setBook] = useState(null);
  const [error, setError] = useState(null);
  const [zoom, setZoom] = useState(null);  // null = авто-окно (см. DepthChart)
  const [now, setNow] = useState(Date.now());
  // Кривые ликвидности по стороне стакана: клик включает расчёт на BFF и отрисовку.
  const [curveOn, setCurveOn] = useState({ 1: false, 2: false, 3: false, 4: false });
  const [slope, setSlope] = useState(null);
  const [curveState, setCurveState] = useState('idle');  // idle | loading | ok | error
  // Живой режим (ADR-070): по умолчанию ВЫКЛ — глубокий опрос грузит venues. Включённая
  // панель шлёт «аренду» пар; перестала слать (выключили/закрыли вкладку) — опрос гаснет сам.
  const [liveOn, setLiveOn] = useState(false);
  const [liveScope, setLiveScope] = useState('pair-all');  // pair-all | venue-all
  const [liveInfo, setLiveInfo] = useState(null);
  // Кривые ВСЕХ бирж по этой валюте и суммарные: клик → расчёт на BFF (через venues).
  const [aggOn, setAggOn] = useState({ venues: false, all: false, ex: false });
  const [aggMethod, setAggMethod] = useState(0);  // 0 = способ, идущий в клиринг (★)
  const [agg, setAgg] = useState(null);
  const [aggState, setAggState] = useState('idle');  // idle | loading | ok | error

  useEffect(() => {
    axios.get(`${API_BASE}/venues/stats`).then((r) => {
      const vs = (r.data && r.data.venues) || [];
      setVenues(vs);
      if (vs.length) {
        setVenue((cur) => cur || vs[0].venue_id);
        setSymbol((cur) => cur || (vs[0].pairs[0] && vs[0].pairs[0].symbol) || '');
      }
    }).catch(() => setError('Не удалось загрузить список бирж'));
  }, []);

  const pairs = useMemo(() => {
    const v = venues.find((x) => x.venue_id === venue);
    return v ? v.pairs.map((p) => p.symbol) : [];
  }, [venues, venue]);

  const load = useCallback(async () => {
    if (!venue || !symbol) return;
    try {
      const r = await axios.get(`${API_BASE}/venues/orderbook`, { params: { venue, symbol, depth: DEPTH } });
      setBook(r.data);
      setError(null);
    } catch (e) {
      setBook(null);
      setError(e.response && e.response.status === 404 ? 'Нет данных стакана' : 'Ошибка загрузки стакана');
    }
  }, [venue, symbol]);

  const anyCurve = METHODS.some((m) => curveOn[m.id]);
  // Номер запроса: запоздалый ответ (после выключения чекбоксов или смены пары) отбрасываем.
  const curveReq = useRef(0);
  const loadCurve = useCallback(async () => {
    if (!venue || !symbol) return;
    const reqId = ++curveReq.current;
    try {
      // Расчёт по сигналу с фронта: BFF → venues считает M1–M4 по глубокому стакану.
      const r = await axios.get(`${API_BASE}/venues/slope-curves`, {
        params: { venue, symbol }, timeout: 15000,
      });
      if (reqId !== curveReq.current) return;
      setSlope(r.data);
      setCurveState('ok');
    } catch (e) {
      if (reqId !== curveReq.current) return;
      setSlope(null);
      const code = e.response && e.response.data && e.response.data.error;
      if (code === 'no_deep_book') setCurveState('nodata');          // у биржи нет глубокого снимка
      else if (code === 'insufficient_book') setCurveState('thin');   // пустая сторона стакана
      else if (code === 'deep_book_stale') setCurveState('stale');          // аренда не активна
      else if (code === 'endpoint_unavailable') setCurveState('unavailable');  // venues без новой ручки
      else setCurveState('error');
    }
  }, [venue, symbol]);

  useEffect(() => {
    if (anyCurve) { setCurveState('loading'); loadCurve(); } else { curveReq.current += 1; setSlope(null); setCurveState('idle'); }
  }, [anyCurve, loadCurve]);
  useInterval(loadCurve, anyCurve ? CURVE_REFRESH_MS : null);
  const liveItems = useMemo(() => {
    if (liveScope === 'venue-all') {
      const v = venues.find((x) => x.venue_id === venue);
      return v ? v.pairs.map((p) => ({ venue: v.venue_id, symbol: p.symbol })) : [];
    }
    // эта пара на всех биржах, где она есть (DEX venues отбросит сам)
    return venues.filter((v) => v.pairs.some((p) => p.symbol === symbol))
      .map((v) => ({ venue: v.venue_id, symbol }));
  }, [venues, venue, symbol, liveScope]);
  const liveReq = useRef(0);
  const sendLive = useCallback(async () => {
    if (!liveItems.length) return;
    const reqId = ++liveReq.current;  // запоздалый ответ после смены пары/выключения отбрасываем
    try {
      const r = await axios.post(`${API_BASE}/venues/depth-watch`, { items: liveItems, ttl_ms: LIVE_LEASE_MS }, { timeout: 8000 });
      if (reqId !== liveReq.current) return;
      setLiveInfo({ ok: true, ...r.data });
    } catch (e) {
      if (reqId !== liveReq.current) return;
      const code = e.response && e.response.data && e.response.data.error;
      setLiveInfo({ ok: false, error: code === 'endpoint_unavailable' ? 'сервис venues не поддерживает живой режим (нужно обновить)' : 'не удалось включить живой режим' });
    }
  }, [liveItems]);
  useEffect(() => { if (liveOn) sendLive(); else { liveReq.current += 1; setLiveInfo(null); } }, [liveOn, sendLive]);
  useInterval(sendLive, liveOn ? LIVE_HEARTBEAT_MS : null);
  const anyAgg = aggOn.venues || aggOn.all || aggOn.ex;
  const aggVenues = useMemo(
    () => venues.filter((v) => v.pairs.some((p) => p.symbol === symbol)).map((v) => v.venue_id),
    [venues, symbol]);
  const aggReq = useRef(0);
  const loadAgg = useCallback(async () => {
    if (!symbol || !aggVenues.length) return;
    const reqId = ++aggReq.current;
    try {
      const r = await axios.get(`${API_BASE}/venues/slope-curves-all`, {
        params: { symbol, venues: aggVenues.join(','), exclude: venue, method: aggMethod || undefined }, timeout: 20000,
      });
      if (reqId !== aggReq.current) return;
      setAgg(r.data);
      setAggState('ok');
    } catch (e) {
      if (reqId !== aggReq.current) return;
      setAgg(null);
      setAggState('error');
    }
  }, [symbol, aggVenues, venue, aggMethod]);
  useEffect(() => {
    if (anyAgg) { setAggState('loading'); loadAgg(); } else { aggReq.current += 1; setAgg(null); setAggState('idle'); }
  }, [anyAgg, loadAgg]);
  useInterval(loadAgg, anyAgg ? CURVE_REFRESH_MS : null);
  // Кривые всех бирж нужны глубокие стаканы ВСЕХ бирж пары ⇒ живой режим «эта пара на всех биржах»
  // включаем сами (пользователь может выключить; нагрузка ограничена потолком аренд).
  useEffect(() => {
    if (anyAgg && (!liveOn || liveScope !== 'pair-all')) { setLiveOn(true); setLiveScope('pair-all'); }
  }, [anyAgg, liveOn, liveScope]);
  const toggleAgg = (key) => setAggOn((a) => ({ ...a, [key]: !a[key] }));
  const toggleCurve = (id) => setCurveOn((c) => ({ ...c, [id]: !c[id] }));

  useEffect(() => { load(); }, [load]);
  useEffect(() => { setZoom(null); }, [venue, symbol]);  // у другого стакана — своё авто-окно
  useInterval(load, REFRESH_MS);
  useInterval(() => setNow(Date.now()), 500);

  const onVenue = (v) => {
    setVenue(v);
    const vv = venues.find((x) => x.venue_id === v);
    if (vv && !vv.pairs.some((p) => p.symbol === symbol)) setSymbol((vv.pairs[0] || {}).symbol || '');
  };

  // cum и vwap считает BFF (CALC-BOOK-VWAP); фронт только рисует.
  const asks = book ? book.asks : [];
  const bids = book ? book.bids : [];
  return (
    <div className="rob">
      <div className="rob-head">
        <select value={venue} onChange={(e) => onVenue(e.target.value)}>
          {venues.map((v) => <option key={v.venue_id} value={v.venue_id}>{v.venue_id}</option>)}
        </select>
        <select value={symbol} onChange={(e) => setSymbol(e.target.value)}>
          {pairs.map((p) => <option key={p} value={p}>{p}</option>)}
        </select>
        <button type="button" className="rob-reset" onClick={() => setZoom(null)} disabled={zoom === null}>Сбросить масштаб</button>
        {book && <span className="rob-age" title="Время снимка стакана на бирже/в venues">
          снимок {Math.max(0, Math.round((now - book.event_time_ms) / 1000))} с назад · {book.bids.length}/{book.asks.length} ур.
          {Math.max(book.bids.length, book.asks.length) <= 50 && ' · только основной снимок (20 ур.): включите «Живой режим» для глубокого стакана'}
        </span>}
      </div>
      <div className="rob-curves">
        <label title="Включает глубокий опрос стакана (до 1000 уровней, ~каждые 2 с) на время, пока панель открыта и режим включён (аренда продлевается каждые 2,5 с). Выключите, когда не нужен — нагрузка на venues.">
          <input type="checkbox" checked={liveOn} onChange={() => setLiveOn((v) => !v)} /> Живой режим (глубокий стакан)
        </label>
        <select value={liveScope} onChange={(e) => setLiveScope(e.target.value)} disabled={!liveOn}>
          <option value="pair-all">эта пара на всех биржах</option>
          <option value="venue-all">все пары этой биржи</option>
        </select>
        {liveOn && liveInfo && liveInfo.ok && (
          <span className="rob-age">
            опрашивается {liveInfo.active} из {liveInfo.max_items} пар
            {liveInfo.rejected_over_cap > 0 && <span className="rob-err"> · {liveInfo.rejected_over_cap} пар не принято (лимит)</span>}
          </span>
        )}
        {liveOn && liveInfo && !liveInfo.ok && <span className="rob-err">{liveInfo.error}</span>}
      </div>
      <div className="rob-curves">
        <span>Кривая ликвидности (прямая P(q)=anchor+β·q):</span>
        {METHODS.map((m) => (
          <label key={m.id} style={{ color: m.color }}>
            <input type="checkbox" checked={curveOn[m.id]} onChange={() => toggleCurve(m.id)} /> {m.label}
            {slope && slope.selectedMethod === m.id ? ' ★' : ''}
          </label>
        ))}
        {anyCurve && curveState === 'loading' && <span className="rob-age">расчёт…</span>}
        {curveState === 'nodata' && <span className="rob-err">для этой биржи/пары нет глубокого стакана — включите «Живой режим»</span>}
        {curveState === 'stale' && <span className="rob-err">глубокий стакан устарел — включите «Живой режим»</span>}
        {curveState === 'thin' && <span className="rob-err">в стакане пустая сторона — кривую не рассчитать</span>}
        {curveState === 'unavailable' && <span className="rob-err">сервис venues не поддерживает расчёт кривых (нужно обновить)</span>}
        {curveState === 'error' && <span className="rob-err">не удалось рассчитать кривую</span>}
      </div>
      <div className="rob-curves">
        <span>По всем биржам ({symbol || '—'}):</span>
        <label><input type="checkbox" checked={aggOn.venues} onChange={() => toggleAgg('venues')} /> кривая каждой биржи</label>
        <label><input type="checkbox" checked={aggOn.all} onChange={() => toggleAgg('all')} /> Σ всех бирж</label>
        <label><input type="checkbox" checked={aggOn.ex} onChange={() => toggleAgg('ex')} /> Σ всех бирж − {venue || 'выбранная'}</label>
        <select value={aggMethod} onChange={(e) => setAggMethod(Number(e.target.value))} disabled={!anyAgg}
          title="Способ снятия наклона §6.2, по которому строятся кривые бирж и суммы">
          <option value={0}>способ: клиринговый ★</option>
          {METHODS.map((m) => <option key={m.id} value={m.id}>способ: {m.label}</option>)}
        </select>
        {anyAgg && aggState === 'loading' && <span className="rob-age">расчёт…</span>}
        {aggState === 'error' && <span className="rob-err">не удалось рассчитать кривые бирж</span>}
        {anyAgg && <span className="rob-age">живой режим включается автоматически (нужны глубокие стаканы всех бирж)</span>}
      </div>
      {error && <div className="rob-msg">{error}</div>}
      {book && (
        <DepthChart
          bids={bids}
          asks={asks}
          zoom={zoom}
          onZoom={setZoom}
          slope={slope && slope.venue === venue && slope.symbol === symbol ? slope : null}
          curveOn={curveOn}
          now={now}
          agg={anyAgg ? agg : null}
          aggOn={aggOn}
          aggVenue={venue}
        />
      )}
    </div>
  );
};

export default RawOrderBook;
