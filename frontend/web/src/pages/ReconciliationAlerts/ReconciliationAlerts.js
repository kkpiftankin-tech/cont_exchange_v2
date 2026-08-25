import React, { useCallback, useDeferredValue, useEffect, useMemo, useState } from 'react';
import NavBar from "../../components/NavBar";
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { isAuthenticated, logout } from '../../api/authService';
import { getReconciliationAlerts } from '../../api/hedgeFlowService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import {
  formatBps,
  formatCurrency,
  formatDateTime,
  formatNumber,
  formatPct,
  formatQty,
  getPnlTone,
  getSlippageTone,
  getStatusTone,
} from '../HedgeFlowMonitor/hedgeFlowUtils';
import './ReconciliationAlerts.css';

const POLL_INTERVAL_MS = 5000;
const ALERT_LIMIT = 80;

const defaultResponse = {
  items: [],
  total: 0,
  summary: {},
  filters: {
    symbols: [],
    venues: [],
    providers: [],
    statuses: [],
    severities: [],
    types: [],
  },
  generatedAt: null,
  source: '',
};

function alertKey(alert) {
  return alert.alertId || `${alert.type}:${alert.hedgeFlowId}`;
}

function shortTime(value) {
  if (!value) return '-';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleTimeString([], {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

function severityTone(severity) {
  if (severity === 'critical') return 'bad';
  if (severity === 'warning') return 'warn';
  if (severity === 'info') return 'info';
  return 'muted';
}

function formatTimeout(ms) {
  const value = Number(ms);
  if (!Number.isFinite(value) || value <= 0) return '-';
  return `${formatNumber(value / 1000, 0)}s`;
}

const ReconciliationAlerts = () => {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const [isAuth, setIsAuth] = useState(null);
  const [alertsResponse, setAlertsResponse] = useState(defaultResponse);
  const [selectedAlertKey, setSelectedAlertKey] = useState('');
  const [isLoading, setIsLoading] = useState(true);
  const [error, setError] = useState('');
  const [severityFilter, setSeverityFilter] = useState('all');
  const [statusFilter, setStatusFilter] = useState('all');
  const [typeFilter, setTypeFilter] = useState('all');
  const [symbolFilter, setSymbolFilter] = useState('all');
  const [venueFilter, setVenueFilter] = useState('all');
  const [providerFilter, setProviderFilter] = useState('all');
  const [searchFilter, setSearchFilter] = useState('');
  const deferredSearch = useDeferredValue(searchFilter);

  useEffect(() => {
    const checkAuth = async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    };
    checkAuth();
  }, [navigate]);

  const requestParams = useMemo(() => ({
    severity: severityFilter,
    status: statusFilter,
    type: typeFilter,
    symbol: symbolFilter,
    venue: venueFilter,
    providerId: providerFilter,
    search: deferredSearch,
    limit: ALERT_LIMIT,
  }), [
    deferredSearch,
    providerFilter,
    severityFilter,
    statusFilter,
    symbolFilter,
    typeFilter,
    venueFilter,
  ]);

  const loadAlerts = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setIsLoading(true);
    try {
      const response = await getReconciliationAlerts(requestParams);
      setAlertsResponse(response);
      setError('');
      setSelectedAlertKey((current) => {
        if (response.items.some((alert) => alertKey(alert) === current)) return current;
        return response.items[0] ? alertKey(response.items[0]) : '';
      });
    } catch (err) {
      setError(t('reconciliationAlerts.states.error'));
    } finally {
      if (showLoader) setIsLoading(false);
    }
  }, [requestParams, t]);

  useEffect(() => {
    if (isAuth) {
      loadAlerts({ showLoader: true });
    }
  }, [isAuth, loadAlerts]);

  useInterval(() => {
    if (isAuth) {
      loadAlerts();
    }
  }, isAuth ? POLL_INTERVAL_MS : null);

  const selectedAlert = useMemo(
    () => alertsResponse.items.find((alert) => alertKey(alert) === selectedAlertKey) || alertsResponse.items[0] || null,
    [alertsResponse.items, selectedAlertKey]
  );

  const summary = alertsResponse.summary || {};
  const summaryCards = [
    {
      id: 'total',
      value: formatNumber(summary.total, 0),
      tone: 'info',
    },
    {
      id: 'critical',
      value: formatNumber(summary.critical, 0),
      tone: (summary.critical || 0) > 0 ? 'bad' : 'good',
    },
    {
      id: 'underfilled',
      value: formatNumber(summary.underfilled, 0),
      tone: (summary.underfilled || 0) > 0 ? 'warn' : 'good',
    },
    {
      id: 'rejected',
      value: formatNumber(summary.rejected, 0),
      tone: (summary.rejected || 0) > 0 ? 'bad' : 'good',
    },
    {
      id: 'gap',
      value: formatNumber(summary.totalGapQty, 4),
      tone: (summary.totalGapQty || 0) > 0 ? 'warn' : 'good',
    },
    {
      id: 'avgGap',
      value: formatPct(summary.avgGapPct),
      tone: (summary.avgGapPct || 0) > 20 ? 'bad' : 'warn',
    },
  ];

  const handleLogout = () => {
    logout();
    navigate('/login');
  };

  if (isAuth === null) {
    return <div className="loading-screen">{t('auth.loading')}</div>;
  }

  return (
    <div className="alerts-page">
      <NavBar />

      <main className="alerts-shell">
        <section className="alerts-hero">
          <div>
            <span className="alerts-kicker">{t('reconciliationAlerts.kicker')}</span>
            <h1>{t('reconciliationAlerts.title')}</h1>
            <p>{t('reconciliationAlerts.subtitle')}</p>
          </div>
          <div className="alerts-hero-meta">
            <button type="button" className="alerts-refresh-btn" onClick={() => loadAlerts({ showLoader: true })}>
              {t('reconciliationAlerts.actions.refresh')}
            </button>
            <div className="alerts-live-pill">
              <span className="alerts-live-dot" />
              {t('reconciliationAlerts.live', { seconds: POLL_INTERVAL_MS / 1000 })}
            </div>
            <div className="alerts-refresh-label">
              {t('reconciliationAlerts.lastUpdated', {
                time: alertsResponse.generatedAt ? formatDateTime(alertsResponse.generatedAt) : '-',
              })}
            </div>
          </div>
        </section>

        <section className="alerts-toolbar">
          <label>
            <span>{t('reconciliationAlerts.filters.severity')}</span>
            <select value={severityFilter} onChange={(event) => setSeverityFilter(event.target.value)}>
              <option value="all">{t('reconciliationAlerts.filters.all')}</option>
              {(alertsResponse.filters?.severities || []).map((severity) => (
                <option key={severity} value={severity}>
                  {t(`reconciliationAlerts.severity.${severity}`, { defaultValue: severity })}
                </option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('reconciliationAlerts.filters.status')}</span>
            <select value={statusFilter} onChange={(event) => setStatusFilter(event.target.value)}>
              <option value="all">{t('reconciliationAlerts.filters.all')}</option>
              {(alertsResponse.filters?.statuses || []).map((status) => (
                <option key={status} value={status}>{status}</option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('reconciliationAlerts.filters.type')}</span>
            <select value={typeFilter} onChange={(event) => setTypeFilter(event.target.value)}>
              <option value="all">{t('reconciliationAlerts.filters.all')}</option>
              {(alertsResponse.filters?.types || []).map((type) => (
                <option key={type} value={type}>{type}</option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('reconciliationAlerts.filters.symbol')}</span>
            <select value={symbolFilter} onChange={(event) => setSymbolFilter(event.target.value)}>
              <option value="all">{t('reconciliationAlerts.filters.all')}</option>
              {(alertsResponse.filters?.symbols || []).map((symbol) => (
                <option key={symbol} value={symbol}>{symbol}</option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('reconciliationAlerts.filters.venue')}</span>
            <select value={venueFilter} onChange={(event) => setVenueFilter(event.target.value)}>
              <option value="all">{t('reconciliationAlerts.filters.all')}</option>
              {(alertsResponse.filters?.venues || []).map((venue) => (
                <option key={venue} value={venue}>{venue}</option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('reconciliationAlerts.filters.provider')}</span>
            <select value={providerFilter} onChange={(event) => setProviderFilter(event.target.value)}>
              <option value="all">{t('reconciliationAlerts.filters.all')}</option>
              {(alertsResponse.filters?.providers || []).map((provider) => (
                <option key={provider} value={provider}>{provider}</option>
              ))}
            </select>
          </label>
          <label className="alerts-search-field">
            <span>{t('reconciliationAlerts.filters.search')}</span>
            <input
              value={searchFilter}
              onChange={(event) => setSearchFilter(event.target.value)}
              placeholder={t('reconciliationAlerts.filters.searchPlaceholder')}
            />
          </label>
          <div className="alerts-source-pill">{alertsResponse.source || 'api'}</div>
        </section>

        <section className="alerts-summary-grid">
          {summaryCards.map((card) => (
            <article className={`alerts-summary-card alerts-value-${card.tone}`} key={card.id}>
              <span>{t(`reconciliationAlerts.summary.${card.id}`)}</span>
              <strong>{card.value}</strong>
              <small>{t(`reconciliationAlerts.summary.${card.id}Meta`)}</small>
            </article>
          ))}
        </section>

        {error && <div className="alerts-empty-state error">{error}</div>}

        <section className="alerts-layout">
          <div className="alerts-list-panel">
            <div className="alerts-panel-title">
              <div>
                <h2>{t('reconciliationAlerts.list.title')}</h2>
                <p>{t('reconciliationAlerts.list.subtitle', { count: alertsResponse.total || 0 })}</p>
              </div>
              <span>{t('reconciliationAlerts.list.topic')}</span>
            </div>

            {isLoading ? (
              <div className="alerts-empty-state">{t('reconciliationAlerts.states.loading')}</div>
            ) : alertsResponse.items.length === 0 ? (
              <div className="alerts-empty-state">{t('reconciliationAlerts.states.empty')}</div>
            ) : (
              <div className="alerts-table">
                <div className="alerts-table-head alerts-row-layout">
                  <span>{t('reconciliationAlerts.table.time')}</span>
                  <span>{t('reconciliationAlerts.table.alert')}</span>
                  <span>{t('reconciliationAlerts.table.flow')}</span>
                  <span>{t('reconciliationAlerts.table.gap')}</span>
                  <span>{t('reconciliationAlerts.table.route')}</span>
                  <span>{t('reconciliationAlerts.table.status')}</span>
                </div>
                <div className="alerts-table-body">
                  {alertsResponse.items.map((alert) => {
                    const key = alertKey(alert);
                    const tone = severityTone(alert.severity);
                    return (
                      <button
                        type="button"
                        className={`alerts-row alerts-row-layout ${selectedAlertKey === key ? 'selected' : ''}`}
                        key={key}
                        onClick={() => setSelectedAlertKey(key)}
                      >
                        <span>
                          <strong>{shortTime(alert.timestamp)}</strong>
                          <small>{formatDateTime(alert.timestamp)}</small>
                        </span>
                        <span>
                          <b className={`alerts-status alerts-status-${tone}`}>
                            {t(`reconciliationAlerts.severity.${alert.severity}`, { defaultValue: alert.severity })}
                          </b>
                          <small>{alert.type}</small>
                        </span>
                        <span>
                          <strong>{alert.hedgeFlowId}</strong>
                          <small>{alert.batchId}</small>
                        </span>
                        <span>
                          <strong>{formatQty(alert.gapQty, alert.symbol)}</strong>
                          <small>{formatPct(alert.gapPct)}</small>
                        </span>
                        <span>
                          <strong>{alert.symbol} / {alert.side}</strong>
                          <small>{alert.venueIds.join(', ') || '-'}</small>
                        </span>
                        <span>
                          <b className={`alerts-status alerts-status-${getStatusTone(alert.status)}`}>{alert.status}</b>
                          <small>{alert.sourceTopic}</small>
                        </span>
                      </button>
                    );
                  })}
                </div>
              </div>
            )}
          </div>

          <aside className="alerts-inspector">
            {selectedAlert ? (
              <>
                <div className="alerts-inspector-hero">
                  <span className="alerts-kicker">{t('reconciliationAlerts.inspector.kicker')}</span>
                  <h2>{selectedAlert.type}</h2>
                  <p>{selectedAlert.hedgeFlowId} / {selectedAlert.sourceTopic}</p>
                </div>

                <div className="alerts-detail-grid">
                  <div>
                    <span>{t('reconciliationAlerts.inspector.gap')}</span>
                    <strong>{formatQty(selectedAlert.gapQty, selectedAlert.symbol)}</strong>
                  </div>
                  <div>
                    <span>{t('reconciliationAlerts.inspector.gapPct')}</span>
                    <strong>{formatPct(selectedAlert.gapPct)}</strong>
                  </div>
                  <div>
                    <span>{t('reconciliationAlerts.inspector.target')}</span>
                    <strong>{formatQty(selectedAlert.targetQty, selectedAlert.symbol)}</strong>
                  </div>
                  <div>
                    <span>{t('reconciliationAlerts.inspector.filled')}</span>
                    <strong>{formatQty(selectedAlert.filledQty, selectedAlert.symbol)}</strong>
                  </div>
                  <div>
                    <span>{t('reconciliationAlerts.inspector.slippage')}</span>
                    <strong className={`alerts-value-${getSlippageTone(selectedAlert.slippageBps)}`}>
                      {formatBps(selectedAlert.slippageBps)}
                    </strong>
                  </div>
                  <div>
                    <span>{t('reconciliationAlerts.inspector.pnl')}</span>
                    <strong className={`alerts-value-${getPnlTone(selectedAlert.hedgePnl)}`}>
                      {formatCurrency(selectedAlert.hedgePnl, selectedAlert.feeCurrency)}
                    </strong>
                  </div>
                </div>

                <div className="alerts-next-action">
                  <span>{t('reconciliationAlerts.inspector.nextAction')}</span>
                  <strong>{selectedAlert.nextAction || selectedAlert.statusReason || '-'}</strong>
                </div>

                <dl className="alerts-facts">
                  <div>
                    <dt>{t('reconciliationAlerts.inspector.batch')}</dt>
                    <dd>{selectedAlert.batchId}</dd>
                  </div>
                  <div>
                    <dt>{t('reconciliationAlerts.inspector.provider')}</dt>
                    <dd>{selectedAlert.providerId}</dd>
                  </div>
                  <div>
                    <dt>{t('reconciliationAlerts.inspector.venues')}</dt>
                    <dd>{selectedAlert.venueIds.join(', ') || '-'}</dd>
                  </div>
                  <div>
                    <dt>{t('reconciliationAlerts.inspector.timeout')}</dt>
                    <dd>{formatTimeout(selectedAlert.timeoutMs)}</dd>
                  </div>
                  <div>
                    <dt>{t('reconciliationAlerts.inspector.risk')}</dt>
                    <dd>{selectedAlert.riskDecision || '-'} / {formatPct(selectedAlert.riskLimitUsagePct)}</dd>
                  </div>
                  <div>
                    <dt>{t('reconciliationAlerts.inspector.mode')}</dt>
                    <dd>{selectedAlert.hedgeMode || '-'}</dd>
                  </div>
                </dl>

                <div className="alerts-action-links">
                  <a className="alerts-flow-link" href={`/manual-override?sourceAlertId=${encodeURIComponent(selectedAlert.alertId)}`}>
                    {t('reconciliationAlerts.actions.openManualOverride')}
                  </a>
                  <a className="alerts-flow-link secondary" href="/hedgeflows">
                    {t('reconciliationAlerts.actions.openHedgeflows')}
                  </a>
                </div>
              </>
            ) : (
              <div className="alerts-empty-state compact">{t('reconciliationAlerts.states.select')}</div>
            )}
          </aside>
        </section>
      </main>
    </div>
  );
};

export default ReconciliationAlerts;
