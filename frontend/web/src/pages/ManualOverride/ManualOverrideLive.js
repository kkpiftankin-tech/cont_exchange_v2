import React, { useCallback, useEffect, useState } from 'react';
import NavBar from "../../components/NavBar";
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated, logout } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import './ManualOverrideLive.css';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 5000;

const ORIGIN_LABEL = {
  f12_auto_hedge: 'F-12 auto-hedge',
  f04_external_fill: 'F-04 external_fill',
  manual: 'Manual',
  unknown: '—'
};

function fmt(value, digits = 6) {
  if (value === null || value === undefined || value === '') return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  return n.toFixed(digits).replace(/0+$/, '').replace(/\.$/, '');
}
function fmtTs(value) {
  if (!value) return '—';
  try { return new Date(value).toLocaleString('ru-RU', { hour12: false }); }
  catch (e) { return String(value); }
}

const ManualOverrideLive = () => {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [data, setData] = useState({ items: [], summary: {}, generatedAt: null });
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [originFilter, setOriginFilter] = useState('all');

  useEffect(() => {
    isAuthenticated().then((auth) => { setIsAuth(auth); if (!auth) navigate('/login'); });
  }, [navigate]);

  const load = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setLoading(true);
    try {
      const response = await axios.get(`${API_BASE}/v1/hedge/manual-overrides`, {
        params: { limit: 50 }, timeout: 6000
      });
      setData(response.data);
      setError('');
    } catch (err) {
      setError(err.response?.data?.message || err.message || 'Ошибка загрузки');
    } finally {
      if (showLoader) setLoading(false);
    }
  }, []);

  useEffect(() => { if (isAuth) load({ showLoader: true }); }, [isAuth, load]);
  useInterval(() => { if (isAuth) load(); }, isAuth ? POLL_INTERVAL_MS : null);

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  const items = (data?.items || []).filter(
    (it) => originFilter === 'all' || it.origin === originFilter
  );
  const summary = data?.summary || {};

  return (
    <div className="mo-page">
      <NavBar />

      <main className="mo-shell">
        <section className="mo-hero">
          <div>
            <span className="mo-kicker">F-12 / DoD-15</span>
            <h1>Manual Override — live</h1>
            <p>Источник: PG <code>hedgeflows</code>. Origin выводится из паттерна intent_id. Polling 5s.</p>
          </div>
          <div className="mo-meta">
            <button type="button" className="mo-refresh" onClick={() => load({ showLoader: true })}>Обновить</button>
            <button type="button" className="mo-create-btn" disabled title="POST /api/v1/hedge/manual-overrides не реализован (отдельный PR)">
              + Создать override
            </button>
            <div className="mo-refresh-label">Обновлено: {data?.generatedAt ? fmtTs(data.generatedAt) : '—'}</div>
          </div>
        </section>

        <section className="mo-banner">
          <strong>ℹ Read-only режим.</strong> Создание manual override (POST → publish в Kafka <code>execution.intents</code>
          с <code>origin=MANUAL</code>) пока недоступно — это отдельный PR (требуется proto-extension + audit trail).
          Здесь показан только листинг с derived origin.
        </section>

        <section className="mo-cards">
          <article className="mo-card"><span>Всего</span><strong>{summary.total ?? 0}</strong><small>в выборке</small></article>
          <article className="mo-card"><span>F-12 auto-hedge</span><strong>{summary.f12AutoHedge ?? 0}</strong><small>|hedge| в intent_id</small></article>
          <article className="mo-card"><span>F-04 external_fill</span><strong>{summary.f04ExternalFill ?? 0}</strong><small>|external_fill_N|</small></article>
          <article className="mo-card mo-card-highlight"><span>Manual</span><strong>{summary.manual ?? 0}</strong><small>не из matching/F-04</small></article>
          <article className="mo-card"><span>Unknown</span><strong>{summary.unknown ?? 0}</strong><small>не классифицировано</small></article>
        </section>

        <section className="mo-toolbar">
          <div className="mo-field">
            <label>Origin</label>
            <select value={originFilter} onChange={(e) => setOriginFilter(e.target.value)}>
              <option value="all">Все</option>
              <option value="manual">Manual</option>
              <option value="f12_auto_hedge">F-12 auto-hedge</option>
              <option value="f04_external_fill">F-04 external_fill</option>
              <option value="unknown">Unknown</option>
            </select>
          </div>
        </section>

        {loading ? (
          <div className="mo-state">Загрузка...</div>
        ) : error ? (
          <div className="mo-state error">{error}</div>
        ) : items.length === 0 ? (
          <div className="mo-state">
            <h3>Нет записей</h3>
            <p>В выборке нет hedge flows. В dev manual override не создаётся, потому что POST endpoint deferred.</p>
          </div>
        ) : (
          <div className="mo-table-wrap">
            <table className="mo-table">
              <thead>
                <tr>
                  <th>Time</th>
                  <th>Origin</th>
                  <th>HedgeFlowId</th>
                  <th>Symbol</th>
                  <th>Side</th>
                  <th>Target</th>
                  <th>Filled</th>
                  <th>Avg Price</th>
                  <th>Urgency</th>
                  <th>Status</th>
                  <th>PnL</th>
                </tr>
              </thead>
              <tbody>
                {items.map((it) => (
                  <tr key={it.hedgeFlowId} className={`origin-${it.origin}`}>
                    <td>{fmtTs(it.createdAt)}</td>
                    <td><span className={`mo-origin-badge origin-${it.origin}`}>{ORIGIN_LABEL[it.origin] || it.origin}</span></td>
                    <td className="mo-id" title={it.hedgeFlowId}>
                      {it.hedgeFlowId.length > 22 ? `${it.hedgeFlowId.slice(0, 22)}…` : it.hedgeFlowId}
                    </td>
                    <td>{it.symbol}</td>
                    <td className={`side-${(it.side || '').toLowerCase()}`}>{it.side}</td>
                    <td>{fmt(it.targetQty)}</td>
                    <td>{fmt(it.filledQty)}</td>
                    <td>{fmt(it.avgFillPrice, 4)}</td>
                    <td>{it.urgency}</td>
                    <td><span className={`hedge-status-badge status-${it.status}`}>{it.status}</span></td>
                    <td className={Number(it.hedgePnl) >= 0 ? 'pnl-pos' : 'pnl-neg'}>{fmt(it.hedgePnl, 4)}</td>
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

export default ManualOverrideLive;
