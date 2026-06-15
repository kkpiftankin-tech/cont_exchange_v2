import React, { useCallback, useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { useTranslation } from 'react-i18next';
import { isAuthenticated, logout } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import HedgeFlowDrillPanel from './HedgeFlowDrillPanel';
// PR-F02-013: use the customer page chrome (Profile.css gives the same
// radial-gradient dark background as Main/Profile) instead of the ops
// monitor's standalone white-ish admin chrome. The hedge-table styling
// from HedgeFlowMonitor.css/HedgeFlowMonitorLive.css is still imported
// for the table cells, badges, drill panel — only the outer page shell
// is swapped.
import '../Profile/Profile.css';
import './HedgeFlowMonitor.css';
import './HedgeFlowMonitorLive.css';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 2000;

const STATUS_FILTERS = [
  'all', 'OPEN', 'COMPLETED', 'UNDERFILLED', 'REJECTED', 'RISK_REJECTED', 'CANCELLED'
];

const PERIOD_FILTERS = [
  { value: 'all', label: 'Все' },
  { value: '1h', label: '1 час' },
  { value: '24h', label: '24 часа' },
  { value: '7d', label: '7 дней' },
];

function formatNumber(value, digits = 6) {
  if (value === null || value === undefined || value === '') return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  return n.toFixed(digits).replace(/0+$/, '').replace(/\.$/, '');
}

function formatPnl(value) {
  if (value === null || value === undefined || value === '') return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  const sign = n > 0 ? '+' : '';
  return `${sign}${n.toFixed(2)}`;
}

function formatDateTime(value) {
  if (!value) return '—';
  try {
    return new Date(value).toLocaleString('ru-RU', { hour12: false });
  } catch (e) {
    return String(value);
  }
}

function formatRatio(filled, target) {
  const f = Number(filled);
  const t = Number(target);
  if (!Number.isFinite(f) || !Number.isFinite(t) || t === 0) return '—';
  return `${((f / t) * 100).toFixed(1)}%`;
}

const HedgeFlowMonitorLive = () => {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [data, setData] = useState({ items: [], summary: {}, generatedAt: null, source: '' });
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [statusFilter, setStatusFilter] = useState('all');
  const [periodFilter, setPeriodFilter] = useState('24h');
  const [selectedFlowId, setSelectedFlowId] = useState(null);
  // PR-F02-010: client-side substring filter over batch_id / hedge_flow_id /
  // intent_id. Server endpoint already supports symbol/status/period query
  // params but not a free-text search; doing this client-side keeps the UI
  // change minimal and works for the common "I have a batch_id from the
  // Profile page, show me its hedge flows" workflow.
  const [searchQuery, setSearchQuery] = useState('');

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
      const params = { limit: 50 };
      if (statusFilter !== 'all') params.status = statusFilter;
      if (periodFilter !== 'all') params.period = periodFilter;
      const response = await axios.get(`${API_BASE}/v1/hedge/flows`, {
        params,
        timeout: 5000,
      });
      setData(response.data || { items: [], summary: {} });
      setError('');
    } catch (err) {
      setError(err.response?.data?.message || err.message || 'Ошибка загрузки');
    } finally {
      if (showLoader) setLoading(false);
    }
  }, [statusFilter, periodFilter]);

  useEffect(() => {
    if (isAuth) load({ showLoader: true });
  }, [isAuth, load]);

  useInterval(() => {
    if (isAuth) load();
  }, isAuth ? POLL_INTERVAL_MS : null);

  const handleLogout = () => {
    logout();
    navigate('/login');
  };

  if (isAuth === null) {
    return <div className="loading-screen">Загрузка...</div>;
  }

  const summary = data.summary || {};
  const rawItems = data.items || [];
  const trimmedQuery = searchQuery.trim().toLowerCase();
  const items = trimmedQuery
    ? rawItems.filter((flow) => {
        const haystack = [
          flow.batchId,
          flow.hedgeFlowId,
          flow.intentId,
          flow.providerId,
          flow.symbol
        ]
          .filter(Boolean)
          .join(' ')
          .toLowerCase();
        return haystack.includes(trimmedQuery);
      })
    : rawItems;

  return (
    // PR-F02-013: use Profile's customer chrome (same dark radial-gradient
    // background, same top nav with all the trader+ops links the user
    // expects from /main and /profile) instead of the standalone admin
    // monitor look. Clicking "hedge" from anywhere lands here without
    // visibly switching apps.
    <div className="profile-container">
      <nav className="navbar-main">
        <div className="logo">
          <img src={logo} alt="Logo" className="logo-purple"/>
          <span>{t('navbar.logo')}</span>
        </div>
        <div className="nav-links">
          <a href="/main">{t('navbar.trade')}</a>
          <a href="/profile">{t('navbar.profile')}</a>
          <a href="/venues">{t('navbar.venues')}</a>
          <a href="/hedge-flows-live" className="active">{t('navbar.hedgeflows')}</a>
          <a href="/hedge-pnl">{t('navbar.hedgePnl')}</a>
          <a href="/execution-live">{t('navbar.executionLive')}</a>
          <a href="/reconciliation-alerts">{t('navbar.reconciliationAlerts')}</a>
          <a href="/manual-override">{t('navbar.manualOverride')}</a>
          <a href="/policy-config">{t('navbar.policyConfig')}</a>
          <a href="/replay">{t('navbar.replay')}</a>
          <a href="/combo-compensation-live">Compensation</a>
          <button onClick={handleLogout} className="logout-btn">{t('navbar.logout')}</button>
        </div>
      </nav>

      <main className="hedge-shell">
        <section className="hedge-hero">
          <div className="hedge-hero-copy">
            <span className="hedge-kicker">F-12 / DoD-14</span>
            <h1>HedgeFlow Monitor — PG live</h1>
            <p>Источник: PostgreSQL <code>hedgeflows</code>. Обновление каждые 2 сек.</p>
          </div>
          <div className="hedge-hero-meta">
            <button type="button" className="hedge-refresh-btn" onClick={() => load({ showLoader: true })}>
              Обновить
            </button>
            <div className="hedge-live-pill">
              <span className="hedge-live-dot" />
              live 2s
            </div>
            <div className="hedge-refresh-label">
              Обновлено: {formatDateTime(data.generatedAt)}
            </div>
          </div>
        </section>

        <section className="hedge-summary-grid">
          <article className="hedge-summary-card">
            <span>Всего</span>
            <strong>{summary.total ?? 0}</strong>
            <small>HedgeFlow в выборке</small>
          </article>
          <article className="hedge-summary-card hedge-summary-open">
            <span>OPEN</span>
            <strong>{summary.open ?? 0}</strong>
            <small>в исполнении</small>
          </article>
          <article className="hedge-summary-card">
            <span>COMPLETED</span>
            <strong>{summary.completed ?? 0}</strong>
            <small>исполнены</small>
          </article>
          <article className="hedge-summary-card">
            <span>UNDERFILLED</span>
            <strong>{summary.underfilled ?? 0}</strong>
            <small>недоисполнены</small>
          </article>
          <article className="hedge-summary-card">
            <span>REJECTED</span>
            <strong>{summary.rejected ?? 0}</strong>
            <small>отклонены</small>
          </article>
        </section>

        <section className="hedge-toolbar">
          <div className="hedge-status-filter">
            {STATUS_FILTERS.map((status) => (
              <button
                type="button"
                key={status}
                className={`hedge-filter-chip ${statusFilter === status ? 'active' : ''}`}
                onClick={() => setStatusFilter(status)}
              >
                {status === 'all' ? 'Все' : status}
              </button>
            ))}
          </div>
          <div className="hedge-field-group">
            <label>
              <span>Период</span>
              <select value={periodFilter} onChange={(e) => setPeriodFilter(e.target.value)}>
                {PERIOD_FILTERS.map((opt) => (
                  <option key={opt.value} value={opt.value}>{opt.label}</option>
                ))}
              </select>
            </label>
            <label>
              <span>Поиск</span>
              <input
                type="text"
                value={searchQuery}
                onChange={(e) => setSearchQuery(e.target.value)}
                placeholder="batch_id / hedge_flow_id / provider"
                style={{
                  minWidth: '280px',
                  padding: '6px 10px',
                  fontFamily: 'monospace',
                  fontSize: '12px'
                }}
              />
            </label>
          </div>
        </section>

        <section className="hedge-list-panel">
          <div className="hedge-panel-title list-title">
            <div>
              <h2>
                HedgeFlows ({items.length}
                {trimmedQuery && rawItems.length !== items.length
                  ? ` / ${rawItems.length}`
                  : ''}
                )
              </h2>
              <p>
                Источник: {data.source || 'postgres:hedgeflows'}
                {trimmedQuery && ` · фильтр: "${searchQuery}"`}
              </p>
            </div>
          </div>

          {loading ? (
            <div className="hedge-empty-state">Загрузка...</div>
          ) : error ? (
            <div className="hedge-empty-state error">{error}</div>
          ) : items.length === 0 ? (
            <div className="hedge-empty-state">Нет данных — таблица hedgeflows пуста или фильтр исключил все строки.</div>
          ) : (
            <div className="hedge-live-table-wrap">
              <table className="hedge-live-table">
                <thead>
                  <tr>
                    <th>Время</th>
                    <th>HedgeFlowId</th>
                    <th>Symbol</th>
                    <th>Side</th>
                    <th>Provider</th>
                    <th>Target qty</th>
                    <th>Filled qty</th>
                    <th>Ratio</th>
                    <th>Ref mid</th>
                    <th>Avg fill</th>
                    <th>Fee</th>
                    <th>HedgePnL</th>
                    <th>Status</th>
                  </tr>
                </thead>
                <tbody>
                  {items.map((flow) => (
                    <tr
                      key={flow.hedgeFlowId}
                      className={`status-${flow.status} hedge-row-clickable`}
                      onClick={() => setSelectedFlowId(flow.hedgeFlowId)}
                      title="Открыть drill-down"
                    >
                      <td>{formatDateTime(flow.createdAt)}</td>
                      <td className="hedge-id" title={flow.hedgeFlowId}>
                        {flow.hedgeFlowId.length > 18
                          ? `${flow.hedgeFlowId.slice(0, 18)}…`
                          : flow.hedgeFlowId}
                      </td>
                      <td>{flow.symbol}</td>
                      <td className={`hedge-side hedge-side-${flow.side?.toLowerCase()}`}>{flow.side}</td>
                      <td>{flow.providerId}</td>
                      <td>{formatNumber(flow.targetQty)}</td>
                      <td>{formatNumber(flow.filledQty)}</td>
                      <td>{formatRatio(flow.filledQty, flow.targetQty)}</td>
                      <td>{formatNumber(flow.referenceMid, 4)}</td>
                      <td>{formatNumber(flow.avgFillPrice, 4)}</td>
                      <td>{formatNumber(flow.totFee, 4)}</td>
                      <td className={Number(flow.hedgePnl) >= 0 ? 'pnl-pos' : 'pnl-neg'}>
                        {formatPnl(flow.hedgePnl)}
                      </td>
                      <td><span className={`hedge-status-badge status-${flow.status}`}>{flow.status}</span></td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </section>
      </main>

      {selectedFlowId && (
        <HedgeFlowDrillPanel
          hedgeFlowId={selectedFlowId}
          onClose={() => setSelectedFlowId(null)}
        />
      )}
    </div>
  );
};

export default HedgeFlowMonitorLive;
