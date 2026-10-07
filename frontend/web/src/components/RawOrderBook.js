import React, { useEffect, useMemo, useState, useCallback, useRef } from 'react';
import axios from 'axios';
import useInterval from '../hooks/useInterval';
import './RawOrderBook.css';

// Сырой стакан биржи (venue_snapshots): график глубины (цена × накопленный объём). Только отображение — расчётов домена нет.
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const REFRESH_MS = 1000;
const DEPTH = 1000;      // максимум уровней с BFF (сколько есть в снимке)

const fmtPrice = (p) => (p >= 100 ? p.toFixed(2) : p >= 1 ? p.toFixed(4) : p.toFixed(6));
const fmtQty = (q) => (q >= 100 ? q.toFixed(2) : q.toFixed(5));

const withCum = (levels) => {
  let c = 0;
  return levels.map((l) => { c += l.q; return { ...l, cum: c }; });
};


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
const DepthChart = ({ bids, asks, zoom, onZoom }) => {
  const svgRef = useRef(null);
  // Колесо мыши — масштаб по цене (как на Binance); non-passive, чтобы не скроллить страницу.
  useEffect(() => {
    const el = svgRef.current;
    if (!el) return undefined;
    const onWheel = (e) => {
      e.preventDefault();
      const f = Math.exp(-Math.max(-300, Math.min(300, e.deltaY)) * ZOOM_SENS);
      onZoom((z) => Math.min(ZOOM_MAX, Math.max(1, z * f)));
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
  const half = Math.max(fullHalf / zoom, spread * 2, mid * 1e-7);
  const pMin = mid - half, pMax = mid + half;
  const vis = (rows) => rows.filter((l) => l.p >= pMin && l.p <= pMax);
  const vb = vis(bids), va = vis(asks);
  const vMax = Math.max(vb.length ? vb[vb.length - 1].cum : 0, va.length ? va[va.length - 1].cum : 0) || 1;
  const x = (p) => ML + ((p - pMin) / (pMax - pMin)) * (W - ML - MR);
  const y = (v) => H - MB - (v / vMax) * (H - MT - MB);
  // Ступенчатая область от лучшей цены вглубь стакана до края окна.
  const area = (rows, edge) => {
    if (!rows.length) return '';
    let d = `M${x(rows[0].p)},${y(0)} L${x(rows[0].p)},${y(rows[0].cum)}`;
    for (let i = 1; i < rows.length; i++) d += ` L${x(rows[i].p)},${y(rows[i - 1].cum)} L${x(rows[i].p)},${y(rows[i].cum)}`;
    const last = rows[rows.length - 1];
    return d + ` L${x(edge)},${y(last.cum)} L${x(edge)},${y(0)} Z`;
  };
  const bps = mid > 0 ? (spread / mid) * 1e4 : 0;
  return (
    <>
      <div className="rob-quote">
        <span className="rob-q-bid">Покупка (bid) {bestBid != null ? fmtPrice(bestBid) : '—'}</span>
        <span className="rob-q-spread">спред {fmtPrice(spread)} ({bps.toFixed(2)} б.п.)</span>
        <span className="rob-q-ask">Продажа (ask) {bestAsk != null ? fmtPrice(bestAsk) : '—'}</span>
      </div>
      <div className="rob-range">
        колесо мыши — масштаб · окно ±{fmtPrice(half)} ({((half / mid) * 100).toFixed(3)}%) · уровней в окне: {vb.length} bid / {va.length} ask
      </div>
      <svg ref={svgRef} className="rob-chart" viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="xMidYMid meet">
        {ticks(0, vMax, 4).map((v) => (
          <g key={`y${v}`}>
            <line x1={ML} x2={W - MR} y1={y(v)} y2={y(v)} className="rob-grid" />
            <text x={W - MR + 4} y={y(v) + 3} className="rob-axis">{fmtQty(v)}</text>
          </g>
        ))}
        {ticks(pMin, pMax, 4).map((p) => (
          <text key={`x${p}`} x={x(p)} y={H - 8} className="rob-axis" textAnchor="middle">{fmtPrice(p)}</text>
        ))}
        {bestBid != null && bestAsk != null && (
          <rect x={x(bestBid)} y={MT} width={Math.max(x(bestAsk) - x(bestBid), 1.5)} height={H - MT - MB} className="rob-spread-band" />
        )}
        <path d={area(vb, pMin)} className="rob-area-bid" />
        <path d={area(va, pMax)} className="rob-area-ask" />
        <line x1={x(mid)} x2={x(mid)} y1={MT} y2={H - MB} className="rob-mid-line" />
        <text x={W - MR + 4} y={MT - 2} className="rob-axis rob-axis-t">Σ объём</text>
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
  const [zoom, setZoom] = useState(1);
  const [now, setNow] = useState(Date.now());

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

  useEffect(() => { load(); }, [load]);
  useInterval(load, REFRESH_MS);
  useInterval(() => setNow(Date.now()), 500);

  const onVenue = (v) => {
    setVenue(v);
    const vv = venues.find((x) => x.venue_id === v);
    if (vv && !vv.pairs.some((p) => p.symbol === symbol)) setSymbol((vv.pairs[0] || {}).symbol || '');
  };

  const asks = book ? withCum(book.asks) : [];
  const bids = book ? withCum(book.bids) : [];
  return (
    <div className="rob">
      <div className="rob-head">
        <select value={venue} onChange={(e) => onVenue(e.target.value)}>
          {venues.map((v) => <option key={v.venue_id} value={v.venue_id}>{v.venue_id}</option>)}
        </select>
        <select value={symbol} onChange={(e) => setSymbol(e.target.value)}>
          {pairs.map((p) => <option key={p} value={p}>{p}</option>)}
        </select>
        <button type="button" className="rob-reset" onClick={() => setZoom(1)} disabled={zoom === 1}>Сбросить масштаб</button>
        {book && <span className="rob-age" title="Время снимка стакана на бирже/в venues">
          снимок {Math.max(0, Math.round((now - book.event_time_ms) / 1000))} с назад · {book.bids.length}/{book.asks.length} ур.
        </span>}
      </div>
      {error && <div className="rob-msg">{error}</div>}
      {book && <DepthChart bids={bids} asks={asks} zoom={zoom} onZoom={setZoom} />}
    </div>
  );
};

export default RawOrderBook;
