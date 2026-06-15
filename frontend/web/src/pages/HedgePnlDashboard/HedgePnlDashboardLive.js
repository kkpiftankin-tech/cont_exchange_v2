import React, { useCallback, useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated, logout } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import './HedgePnlDashboardLive.css';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 5000;

const PERIOD_OPTIONS = [
  { value: '1h', label: '1 час' },
  { value: '24h', label: '24 часа' },
  { value: '7d', label: '7 дней' },
  { value: '30d', label: '30 дней' }
];

function fmt(value, digits = 4) {
  if (value === null || value === undefined) return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  return n.toFixed(digits).replace(/0+$/, '').replace(/\.$/, '');
}

function fmtMoney(value, digits = 2) {
  if (value === null || value === undefined) return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  return n.toFixed(digits);
}

function fmtPnl(value) {
  if (value === null || value === undefined) return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  return `${n > 0 ? '+' : ''}${n.toFixed(4)}`;
}

function fmtHour(iso) {
  if (!iso) return '—';
  try {
    return new Date(iso).toLocaleString('ru-RU', { hour12: false, month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit' });
  } catch (e) {
    return String(iso);
  }
}

function PnlSparkline({ data, height = 64 }) {
  if (!data || data.length < 2) {
    return <div className="hedge-pnl-spark-empty">Недостаточно данных для графика</div>;
  }
  const values = data.map((d) => Number(d.cumulativePnl) || 0);
  const min = Math.min(0, ...values);
  const max = Math.max(0, ...values);
  const range = max - min || 1;
  const width = 600;
  const stepX = data.length > 1 ? width / (data.length - 1) : 0;
  const points = values.map((v, i) => {
    const x = i * stepX;
    const y = height - ((v - min) / range) * height;
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(' ');
  const zeroY = height - ((0 - min) / range) * height;

  return (
    <svg viewBox={`0 0 ${width} ${height}`} className="hedge-pnl-spark">
      <line x1="0" x2={width} y1={zeroY} y2={zeroY} stroke="rgba(255,255,255,0.12)" strokeDasharray="3,3" />
      <polyline points={points} fill="none" stroke="#b48cff" strokeWidth="2" />
    </svg>
  );
}

const HedgePnlDashboardLive = () => {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [data, setData] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [period, setPeriod] = useState('24h');
  const [symbolFilter, setSymbolFilter] = useState('all');
  const [venueFilter, setVenueFilter] = useState('all');

  useEffect(() => {
    const checkAuth = async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    };
    checkAuth();
  }, [navigate]);

  const load = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setLoading(true);
    try {
      const params = { period };
      if (symbolFilter !== 'all') params.symbol = symbolFilter;
      if (venueFilter !== 'all') params.venue = venueFilter;
      const response = await axios.get(`${API_BASE}/v1/hedge/pnl`, {
        params, timeout: 8000
      });
      setData(response.data);
      setError('');
    } catch (err) {
      setError(err.response?.data?.message || err.message || 'Ошибка загрузки');
    } finally {
      if (showLoader) setLoading(false);
    }
  }, [period, symbolFilter, venueFilter]);

  useEffect(() => {
    if (isAuth) load({ showLoader: true });
  }, [isAuth, load]);

  useInterval(() => {
    if (isAuth) load();
  }, isAuth ? POLL_INTERVAL_MS : null);

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  const summary = data?.summary || {};
  const timeSeries = data?.timeSeries || [];
  const symbolBreakdown = data?.symbolBreakdown || [];
  const venueBreakdown = data?.venueBreakdown || [];
  const filters = data?.filters || { symbols: [], venues: [], providers: [] };

  return (
    <div className="hedge-pnl-page">
      <nav className="navbar-main hedge-navbar">
        <div className="logo">
          <img src={logo} alt="Logo" className="logo-purple" />
          <span>CEX</span>
        </div>
        <div className="nav-links">
          <a href="/main">Торговля</a>
          <a href="/profile">Профиль</a>
          <a href="/venues">Площадки</a>
          <a href="/hedgeflows">HedgeFlow (mock)</a>
          <a href="/hedge-flows-live">HedgeFlow (live)</a>
          <a href="/hedge-pnl">PnL (mock)</a>
          <a href="/hedge-pnl-live" className="active">PnL (live)</a>
          <a href="/execution-live">Execution feed</a>
          <a href="/reconciliation-alerts">Alerts</a>
          <a href="/manual-override">Manual override</a>
          <a href="/policy-config">Policy</a>
          <a href="/combo-compensation-live">Compensation</a>
          <button onClick={() => { logout(); navigate('/login'); }} className="logout-btn">Выйти</button>
        </div>
      </nav>

      <main className="hedge-pnl-shell">
        <section className="hedge-pnl-hero">
          <div>
            <span className="hedge-pnl-kicker">F-12 / DoD-15</span>
            <h1>Hedge PnL Dashboard — live</h1>
            <p>Источник: ClickHouse <code>execution_reports</code>. Обновление каждые 5 сек.</p>
          </div>
          <div className="hedge-pnl-meta">
            <button type="button" onClick={() => load({ showLoader: true })} className="hedge-pnl-refresh">
              Обновить
            </button>
            <div className="hedge-pnl-live-pill">
              <span className="hedge-pnl-live-dot" /> live 5s
            </div>
            <div className="hedge-pnl-refresh-label">
              Обновлено: {data?.generatedAt ? new Date(data.generatedAt).toLocaleString('ru-RU', { hour12: false }) : '—'}
            </div>
          </div>
        </section>

        <section className="hedge-pnl-toolbar">
          <div className="hedge-pnl-field">
            <label>Период</label>
            <select value={period} onChange={(e) => setPeriod(e.target.value)}>
              {PERIOD_OPTIONS.map((opt) => (
                <option key={opt.value} value={opt.value}>{opt.label}</option>
              ))}
            </select>
          </div>
          <div className="hedge-pnl-field">
            <label>Symbol</label>
            <select value={symbolFilter} onChange={(e) => setSymbolFilter(e.target.value)}>
              <option value="all">Все</option>
              {filters.symbols.map((s) => <option key={s} value={s}>{s}</option>)}
            </select>
          </div>
          <div className="hedge-pnl-field">
            <label>Venue</label>
            <select value={venueFilter} onChange={(e) => setVenueFilter(e.target.value)}>
              <option value="all">Все</option>
              {filters.venues.map((v) => <option key={v} value={v}>{v}</option>)}
            </select>
          </div>
        </section>

        {loading ? (
          <div className="hedge-pnl-state">Загрузка...</div>
        ) : error ? (
          <div className="hedge-pnl-state error">{error}</div>
        ) : (
          <>
            <section className="hedge-pnl-cards">
              <article className="hedge-pnl-card">
                <span>HedgeFlow</span>
                <strong>{summary.totalFlows ?? 0}</strong>
                <small>уникальных</small>
              </article>
              <article className="hedge-pnl-card">
                <span>Reports</span>
                <strong>{summary.totalReports ?? 0}</strong>
                <small>{summary.filledReports ?? 0} FILLED</small>
              </article>
              <article className="hedge-pnl-card">
                <span>Filled Qty</span>
                <strong>{fmt(summary.totalFilledQty, 6)}</strong>
                <small>сумма executed</small>
              </article>
              <article className="hedge-pnl-card">
                <span>Notional</span>
                <strong>{fmtMoney(summary.totalNotional)}</strong>
                <small>USDT equivalent</small>
              </article>
              <article className={`hedge-pnl-card ${Number(summary.totalPnl) >= 0 ? 'pos' : 'neg'}`}>
                <span>Hedge PnL</span>
                <strong>{fmtPnl(summary.totalPnl)}</strong>
                <small>gross</small>
              </article>
              <article className="hedge-pnl-card">
                <span>Fees</span>
                <strong>{fmtMoney(summary.totalFees, 4)}</strong>
                <small>сумма</small>
              </article>
              <article className={`hedge-pnl-card ${Number(summary.netAfterFees) >= 0 ? 'pos' : 'neg'}`}>
                <span>Net After Fees</span>
                <strong>{fmtPnl(summary.netAfterFees)}</strong>
                <small>PnL − fees</small>
              </article>
              <article className="hedge-pnl-card">
                <span>Avg Slippage</span>
                <strong>{fmt(summary.avgSlippageBps, 1)}</strong>
                <small>bps</small>
              </article>
            </section>

            <section className="hedge-pnl-chart-card">
              <h3>Cumulative PnL by hour</h3>
              <PnlSparkline data={timeSeries} />
              <div className="hedge-pnl-chart-x">
                <span>{timeSeries[0]?.hour ? fmtHour(timeSeries[0].hour) : ''}</span>
                <span>{timeSeries.length > 0 ? fmtHour(timeSeries[timeSeries.length - 1].hour) : ''}</span>
              </div>
            </section>

            <section className="hedge-pnl-breakdowns">
              <div className="hedge-pnl-breakdown">
                <h3>По символам ({symbolBreakdown.length})</h3>
                {symbolBreakdown.length === 0 ? (
                  <div className="hedge-pnl-state">Нет данных</div>
                ) : (
                  <table className="hedge-pnl-table">
                    <thead>
                      <tr>
                        <th>Symbol</th>
                        <th>Flows</th>
                        <th>Reports</th>
                        <th>Filled</th>
                        <th>Notional</th>
                        <th>PnL</th>
                        <th>Fees</th>
                        <th>Slip bps</th>
                      </tr>
                    </thead>
                    <tbody>
                      {symbolBreakdown.map((row) => (
                        <tr key={row.symbol}>
                          <td className="strong">{row.symbol}</td>
                          <td>{row.flows}</td>
                          <td>{row.reports}</td>
                          <td>{fmt(row.filledQty, 6)}</td>
                          <td>{fmtMoney(row.notional)}</td>
                          <td className={Number(row.pnl) >= 0 ? 'pnl-pos' : 'pnl-neg'}>{fmtPnl(row.pnl)}</td>
                          <td>{fmtMoney(row.fees, 4)}</td>
                          <td>{fmt(row.avgSlippageBps, 1)}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                )}
              </div>

              <div className="hedge-pnl-breakdown">
                <h3>По venues ({venueBreakdown.length})</h3>
                {venueBreakdown.length === 0 ? (
                  <div className="hedge-pnl-state">Нет данных</div>
                ) : (
                  <table className="hedge-pnl-table">
                    <thead>
                      <tr>
                        <th>Venue</th>
                        <th>Flows</th>
                        <th>Reports</th>
                        <th>Filled</th>
                        <th>Notional</th>
                        <th>PnL</th>
                        <th>Fees</th>
                      </tr>
                    </thead>
                    <tbody>
                      {venueBreakdown.map((row) => (
                        <tr key={row.venueId}>
                          <td className="strong">{row.venueId}</td>
                          <td>{row.flows}</td>
                          <td>{row.reports}</td>
                          <td>{fmt(row.filledQty, 6)}</td>
                          <td>{fmtMoney(row.notional)}</td>
                          <td className={Number(row.pnl) >= 0 ? 'pnl-pos' : 'pnl-neg'}>{fmtPnl(row.pnl)}</td>
                          <td>{fmtMoney(row.fees, 4)}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                )}
              </div>
            </section>
          </>
        )}
      </main>
    </div>
  );
};

export default HedgePnlDashboardLive;
