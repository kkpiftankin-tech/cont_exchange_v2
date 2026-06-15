import React, { useCallback, useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated, logout } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import './ExecutionLiveFeedLive.css';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 3000;

function fmt(value, digits = 6) {
  if (value === null || value === undefined) return '—';
  const n = Number(value);
  if (!Number.isFinite(n) || n === 0) return digits > 0 ? '0' : '—';
  return n.toFixed(digits).replace(/0+$/, '').replace(/\.$/, '');
}
function fmtTime(iso) {
  if (!iso) return '—';
  try {
    const d = new Date(iso);
    return d.toLocaleTimeString('ru-RU', { hour12: false }) + '.' + String(d.getMilliseconds()).padStart(3, '0');
  } catch (e) { return String(iso); }
}

const ExecutionLiveFeedLive = () => {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [data, setData] = useState({ items: [], filters: { venues: [], symbols: [], statuses: [] }, generatedAt: null });
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [venueFilter, setVenueFilter] = useState('all');
  const [symbolFilter, setSymbolFilter] = useState('all');
  const [statusFilter, setStatusFilter] = useState('all');
  const [paused, setPaused] = useState(false);
  const [lastEventTimeMs, setLastEventTimeMs] = useState(0);

  useEffect(() => {
    isAuthenticated().then((auth) => {
      setIsAuth(auth);
      if (!auth) navigate('/login');
    });
  }, [navigate]);

  const load = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setLoading(true);
    try {
      const params = { limit: 100 };
      if (venueFilter !== 'all') params.venue = venueFilter;
      if (symbolFilter !== 'all') params.symbol = symbolFilter;
      if (statusFilter !== 'all') params.status = statusFilter;
      const response = await axios.get(`${API_BASE}/v1/execution/recent`, { params, timeout: 6000 });
      setData(response.data);
      if (response.data?.items?.length > 0) {
        setLastEventTimeMs(response.data.items[0].eventTimeMs);
      }
      setError('');
    } catch (err) {
      setError(err.response?.data?.message || err.message || 'Ошибка загрузки');
    } finally {
      if (showLoader) setLoading(false);
    }
  }, [venueFilter, symbolFilter, statusFilter]);

  useEffect(() => { if (isAuth) load({ showLoader: true }); }, [isAuth, load]);
  useInterval(() => { if (isAuth && !paused) load(); }, isAuth ? POLL_INTERVAL_MS : null);

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  const items = data?.items || [];
  const filters = data?.filters || { venues: [], symbols: [], statuses: [] };

  return (
    <div className="exec-page">
      <nav className="navbar-main hedge-navbar">
        <div className="logo">
          <img src={logo} alt="Logo" className="logo-purple" />
          <span>CEX</span>
        </div>
        <div className="nav-links">
          <a href="/main">Торговля</a>
          <a href="/profile">Профиль</a>
          <a href="/venues">Площадки</a>
          <a href="/hedge-flows-live">HedgeFlow</a>
          <a href="/hedge-pnl-live">PnL</a>
          <a href="/execution-live-feed-live" className="active">Execution</a>
          <a href="/reconciliation-alerts-live">Alerts</a>
          <a href="/manual-override">Manual override</a>
          <a href="/policy-config">Policy</a>
          <a href="/combo-order-live">Combo</a>
          <a href="/combo-compensation-live">Compensation</a>
          <button onClick={() => { logout(); navigate('/login'); }} className="logout-btn">Выйти</button>
        </div>
      </nav>

      <main className="exec-shell">
        <section className="exec-hero">
          <div>
            <span className="exec-kicker">F-12 / DoD-15</span>
            <h1>Execution Live Feed — live</h1>
            <p>Источник: ClickHouse <code>execution_reports</code>. Polling 3s (SSE отложен в отдельный PR).</p>
          </div>
          <div className="exec-meta">
            <button type="button" onClick={() => setPaused((p) => !p)} className={`exec-pause-btn ${paused ? 'paused' : ''}`}>
              {paused ? '▶ Resume' : '❚❚ Pause'}
            </button>
            <button type="button" onClick={() => load({ showLoader: true })} className="exec-refresh">Обновить</button>
            <div className={`exec-live-pill ${paused ? 'paused' : ''}`}>
              <span className="exec-live-dot" /> {paused ? 'paused' : 'live 3s'}
            </div>
            <div className="exec-refresh-label">
              {items.length > 0 ? `${items.length} последних` : 'нет данных'}
            </div>
          </div>
        </section>

        <section className="exec-toolbar">
          <div className="exec-field">
            <label>Venue</label>
            <select value={venueFilter} onChange={(e) => setVenueFilter(e.target.value)}>
              <option value="all">Все</option>
              {filters.venues.map((v) => <option key={v} value={v}>{v}</option>)}
            </select>
          </div>
          <div className="exec-field">
            <label>Symbol</label>
            <select value={symbolFilter} onChange={(e) => setSymbolFilter(e.target.value)}>
              <option value="all">Все</option>
              {filters.symbols.map((s) => <option key={s} value={s}>{s}</option>)}
            </select>
          </div>
          <div className="exec-field">
            <label>Status</label>
            <select value={statusFilter} onChange={(e) => setStatusFilter(e.target.value)}>
              <option value="all">Все</option>
              {filters.statuses.map((s) => <option key={s} value={s}>{s}</option>)}
            </select>
          </div>
        </section>

        {loading ? (
          <div className="exec-state">Загрузка...</div>
        ) : error ? (
          <div className="exec-state error">{error}</div>
        ) : items.length === 0 ? (
          <div className="exec-state">Нет execution reports в выборке.</div>
        ) : (
          <div className="exec-table-wrap">
            <table className="exec-table">
              <thead>
                <tr>
                  <th>Time</th>
                  <th>Venue</th>
                  <th>Symbol</th>
                  <th>Side</th>
                  <th>Status</th>
                  <th>Filled</th>
                  <th>Remaining</th>
                  <th>Avg Price</th>
                  <th>Ref Mid</th>
                  <th>Slip bps</th>
                  <th>Fee</th>
                  <th>HedgePnL</th>
                  <th>Intent</th>
                </tr>
              </thead>
              <tbody>
                {items.map((ev, idx) => (
                  <tr
                    key={ev.reportId}
                    className={`exec-row ${ev.eventTimeMs === lastEventTimeMs && idx === 0 ? 'exec-row-fresh' : ''}`}
                  >
                    <td className="exec-time">{fmtTime(ev.eventTime)}</td>
                    <td>{ev.venueId || '—'}</td>
                    <td>{ev.symbol}</td>
                    <td className={`side-${(ev.side || '').toLowerCase()}`}>{ev.side || '—'}</td>
                    <td><span className={`hedge-status-badge status-${ev.status}`}>{ev.status}</span></td>
                    <td>{fmt(ev.filledQty)}</td>
                    <td>{fmt(ev.remainingQty)}</td>
                    <td>{fmt(ev.avgPrice, 4)}</td>
                    <td>{fmt(ev.referenceMid, 4)}</td>
                    <td>{ev.slippageBps != null ? ev.slippageBps : '—'}</td>
                    <td>{fmt(ev.feeAmount, 4)}</td>
                    <td className={Number(ev.hedgePnl) >= 0 ? 'pnl-pos' : 'pnl-neg'}>
                      {fmt(ev.hedgePnl, 4)}
                    </td>
                    <td className="exec-intent" title={ev.intentId}>
                      {ev.intentId && ev.intentId.length > 18 ? `${ev.intentId.slice(0, 18)}…` : (ev.intentId || '—')}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </main>
    </div>
  );
};

export default ExecutionLiveFeedLive;
