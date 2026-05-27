import React, { useCallback, useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated, logout } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import './ReconciliationAlertsLive.css';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 5000;

function fmt(value, digits = 6) {
  if (value === null || value === undefined || value === '') return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  return n.toFixed(digits).replace(/0+$/, '').replace(/\.$/, '');
}

function fmtTs(value) {
  if (!value) return '—';
  try {
    return new Date(value).toLocaleString('ru-RU', { hour12: false });
  } catch (e) { return String(value); }
}

const ReconciliationAlertsLive = () => {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [data, setData] = useState({ items: [], summary: {}, generatedAt: null });
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [symbolFilter, setSymbolFilter] = useState('all');
  const [includeAcked, setIncludeAcked] = useState(false);
  const [ackingId, setAckingId] = useState(null);

  useEffect(() => {
    isAuthenticated().then((auth) => {
      setIsAuth(auth);
      if (!auth) navigate('/login');
    });
  }, [navigate]);

  const load = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setLoading(true);
    try {
      const params = {};
      if (symbolFilter !== 'all') params.symbol = symbolFilter;
      if (includeAcked) params.includeAcked = 1;
      const response = await axios.get(`${API_BASE}/v1/hedge/reconciliation-alerts`, {
        params, timeout: 6000
      });
      setData(response.data);
      setError('');
    } catch (err) {
      setError(err.response?.data?.message || err.message || 'Ошибка загрузки');
    } finally {
      if (showLoader) setLoading(false);
    }
  }, [symbolFilter, includeAcked]);

  useEffect(() => { if (isAuth) load({ showLoader: true }); }, [isAuth, load]);
  useInterval(() => { if (isAuth) load(); }, isAuth ? POLL_INTERVAL_MS : null);

  const acknowledge = async (hedgeFlowId) => {
    setAckingId(hedgeFlowId);
    try {
      await axios.post(
        `${API_BASE}/v1/hedge/reconciliation-alerts/${encodeURIComponent(hedgeFlowId)}/acknowledge`,
        {}, { timeout: 3000 }
      );
      await load();
    } catch (err) {
      console.error('ack failed:', err);
    } finally {
      setAckingId(null);
    }
  };

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  const summary = data?.summary || {};
  const items = data?.items || [];
  const symbols = Array.from(new Set(items.map((it) => it.symbol))).sort();

  return (
    <div className="rec-page">
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
          <a href="/execution-live">Execution</a>
          <a href="/reconciliation-alerts-live" className="active">Alerts</a>
          <a href="/manual-override">Manual override</a>
          <a href="/policy-config">Policy</a>
          <button onClick={() => { logout(); navigate('/login'); }} className="logout-btn">Выйти</button>
        </div>
      </nav>

      <main className="rec-shell">
        <section className="rec-hero">
          <div>
            <span className="rec-kicker">F-12 / DoD-15</span>
            <h1>Reconciliation Alerts — live</h1>
            <p>Источник: PostgreSQL <code>hedgeflows</code> WHERE status IN (UNDERFILLED, REJECTED, RISK_REJECTED). Polling 5s.</p>
          </div>
          <div className="rec-meta">
            <button type="button" onClick={() => load({ showLoader: true })} className="rec-refresh">Обновить</button>
            <div className="rec-live-pill"><span className="rec-live-dot" /> live 5s</div>
            <div className="rec-refresh-label">
              Обновлено: {data?.generatedAt ? fmtTs(data.generatedAt) : '—'}
            </div>
          </div>
        </section>

        <section className="rec-cards">
          <article className="rec-card"><span>Всего</span><strong>{summary.total ?? 0}</strong><small>активных</small></article>
          <article className="rec-card crit"><span>CRITICAL</span><strong>{summary.critical ?? 0}</strong><small>REJECTED + RISK_REJECTED</small></article>
          <article className="rec-card warn"><span>WARNING</span><strong>{summary.warning ?? 0}</strong><small>UNDERFILLED</small></article>
          <article className="rec-card"><span>UNDERFILLED</span><strong>{summary.underfilled ?? 0}</strong><small>недоисполнены</small></article>
          <article className="rec-card"><span>REJECTED</span><strong>{summary.rejected ?? 0}</strong><small>отказ венью</small></article>
          <article className="rec-card"><span>RISK_REJECTED</span><strong>{summary.riskRejected ?? 0}</strong><small>отказ риском</small></article>
          <article className="rec-card"><span>ACK (сессия)</span><strong>{summary.acknowledgedInSession ?? 0}</strong><small>после рестарта обнулится</small></article>
        </section>

        <section className="rec-toolbar">
          <div className="rec-field">
            <label>Symbol</label>
            <select value={symbolFilter} onChange={(e) => setSymbolFilter(e.target.value)}>
              <option value="all">Все</option>
              {symbols.map((s) => <option key={s} value={s}>{s}</option>)}
            </select>
          </div>
          <label className="rec-toggle">
            <input
              type="checkbox"
              checked={includeAcked}
              onChange={(e) => setIncludeAcked(e.target.checked)}
            />
            <span>Показать acknowledged</span>
          </label>
        </section>

        {loading ? (
          <div className="rec-state">Загрузка...</div>
        ) : error ? (
          <div className="rec-state error">{error}</div>
        ) : items.length === 0 ? (
          <div className="rec-state">
            <h3>Нет активных alerts</h3>
            <p>В таблице <code>hedgeflows</code> нет строк со статусом UNDERFILLED/REJECTED/RISK_REJECTED.</p>
            <p className="rec-note">В dev это нормально — venues sim адаптер всегда успешно исполняет hedge intents (см. knownIssue <code>venues-sim-zero-execution-price</code>).</p>
          </div>
        ) : (
          <div className="rec-table-wrap">
            <table className="rec-table">
              <thead>
                <tr>
                  <th>Severity</th>
                  <th>Time</th>
                  <th>HedgeFlowId</th>
                  <th>Symbol</th>
                  <th>Side</th>
                  <th>Target</th>
                  <th>Filled</th>
                  <th>Gap</th>
                  <th>Gap %</th>
                  <th>Status</th>
                  <th>Error</th>
                  <th>Действие</th>
                </tr>
              </thead>
              <tbody>
                {items.map((alert) => (
                  <tr key={alert.hedgeFlowId} className={`sev-${alert.severity} ${alert.acknowledged ? 'acked' : ''}`}>
                    <td>
                      <span className={`rec-sev-badge ${alert.severity}`}>{alert.severity}</span>
                    </td>
                    <td>{fmtTs(alert.updatedAt)}</td>
                    <td className="rec-id" title={alert.hedgeFlowId}>
                      {alert.hedgeFlowId.length > 22 ? `${alert.hedgeFlowId.slice(0, 22)}…` : alert.hedgeFlowId}
                    </td>
                    <td>{alert.symbol}</td>
                    <td className={`side-${(alert.side || '').toLowerCase()}`}>{alert.side}</td>
                    <td>{fmt(alert.targetQty)}</td>
                    <td>{fmt(alert.filledQty)}</td>
                    <td>{fmt(alert.gapQty)}</td>
                    <td>{alert.gapPct != null ? `${alert.gapPct}%` : '—'}</td>
                    <td><span className={`hedge-status-badge status-${alert.status}`}>{alert.status}</span></td>
                    <td className="rec-error" title={alert.errorMessage || ''}>
                      {alert.errorCode || '—'}
                    </td>
                    <td>
                      {alert.acknowledged ? (
                        <span className="rec-acked-label">ACK</span>
                      ) : (
                        <button
                          type="button"
                          className="rec-ack-btn"
                          disabled={ackingId === alert.hedgeFlowId}
                          onClick={() => acknowledge(alert.hedgeFlowId)}
                        >
                          {ackingId === alert.hedgeFlowId ? '…' : 'Acknowledge'}
                        </button>
                      )}
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

export default ReconciliationAlertsLive;
