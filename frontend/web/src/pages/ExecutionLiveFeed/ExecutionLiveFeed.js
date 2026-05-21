import React, { useCallback, useDeferredValue, useEffect, useMemo, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { isAuthenticated, logout } from '../../api/authService';
import {
  getExecutionLiveSnapshot,
  subscribeExecutionLiveFeed,
} from '../../api/executionLiveFeedService';
import logo from '../../assets/logo-purple.svg';
import {
  formatBps,
  formatCurrency,
  formatDateTime,
  formatNumber,
  formatPrice,
  formatQty,
  getPnlTone,
  getSlippageTone,
  getStatusTone,
} from '../HedgeFlowMonitor/hedgeFlowUtils';
import './ExecutionLiveFeed.css';

const SNAPSHOT_LIMIT = 50;
const MAX_EVENTS = 90;

const emptyFeed = {
  items: [],
  total: 0,
  summary: {},
  filters: {
    symbols: [],
    venues: [],
    providers: [],
    statuses: [],
    sides: [],
  },
  generatedAt: null,
  source: '',
};

function eventKey(row) {
  return `${row.sequence || 'snapshot'}-${row.executionId}-${row.timestamp}`;
}

function mergeFeedEvent(current, next) {
  const nextKey = eventKey(next);
  return [next, ...current.filter((event) => eventKey(event) !== nextKey)].slice(0, MAX_EVENTS);
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

function connectionTone(state) {
  if (state === 'connected') return 'good';
  if (state === 'connecting' || state === 'reconnecting') return 'info';
  if (state === 'paused') return 'warn';
  if (state === 'error') return 'bad';
  return 'muted';
}

const ExecutionLiveFeed = () => {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const [isAuth, setIsAuth] = useState(null);
  const [feed, setFeed] = useState(emptyFeed);
  const [events, setEvents] = useState([]);
  const [selectedEventKey, setSelectedEventKey] = useState('');
  const [isLoading, setIsLoading] = useState(true);
  const [error, setError] = useState('');
  const [connectionState, setConnectionState] = useState('idle');
  const [lastEventAt, setLastEventAt] = useState(null);
  const [paused, setPaused] = useState(false);
  const [symbolFilter, setSymbolFilter] = useState('all');
  const [venueFilter, setVenueFilter] = useState('all');
  const [providerFilter, setProviderFilter] = useState('all');
  const [statusFilter, setStatusFilter] = useState('all');
  const [sideFilter, setSideFilter] = useState('all');
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
    symbol: symbolFilter,
    venue: venueFilter,
    providerId: providerFilter,
    status: statusFilter,
    side: sideFilter,
    search: deferredSearch,
    limit: SNAPSHOT_LIMIT,
  }), [deferredSearch, providerFilter, sideFilter, statusFilter, symbolFilter, venueFilter]);

  const loadSnapshot = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setIsLoading(true);
    try {
      const response = await getExecutionLiveSnapshot(requestParams);
      setFeed(response);
      setEvents(response.items || []);
      setLastEventAt(response.generatedAt || null);
      setError('');
    } catch (err) {
      setError(t('executionLive.states.error'));
    } finally {
      if (showLoader) setIsLoading(false);
    }
  }, [requestParams, t]);

  useEffect(() => {
    if (isAuth) {
      loadSnapshot({ showLoader: true });
    }
  }, [isAuth, loadSnapshot]);

  useEffect(() => {
    if (!isAuth) return undefined;
    if (paused) {
      setConnectionState('paused');
      return undefined;
    }

    return subscribeExecutionLiveFeed({
      params: requestParams,
      onState: setConnectionState,
      onOpen: () => setError(''),
      onSnapshot: (payload = emptyFeed) => {
        setFeed(payload);
        setEvents(payload.items || []);
        setLastEventAt(payload.generatedAt || null);
      },
      onReport: (item, message) => {
        setEvents((current) => mergeFeedEvent(current, item));
        setFeed((current) => ({
          ...current,
          generatedAt: message.generatedAt || item.timestamp,
          source: message.source || current.source,
        }));
        setLastEventAt(message.generatedAt || item.timestamp);
      },
      onHeartbeat: (message) => {
        setLastEventAt(message.generatedAt || new Date().toISOString());
      },
      onError: (err) => {
        const key = err instanceof SyntaxError
          ? 'executionLive.states.badMessage'
          : 'executionLive.states.streamError';
        setError(t(key));
      },
    });
  }, [isAuth, paused, requestParams, t]);

  useEffect(() => {
    if (events.length === 0) {
      setSelectedEventKey('');
      return;
    }
    if (!events.some((event) => eventKey(event) === selectedEventKey)) {
      setSelectedEventKey(eventKey(events[0]));
    }
  }, [events, selectedEventKey]);

  const selectedEvent = useMemo(
    () => events.find((event) => eventKey(event) === selectedEventKey) || events[0] || null,
    [events, selectedEventKey]
  );

  const summary = feed.summary || {};
  const summaryCards = [
    {
      id: 'reports',
      value: formatNumber(summary.reports, 0),
      meta: t('executionLive.summary.reportsMeta'),
      tone: 'info',
    },
    {
      id: 'notional',
      value: formatCurrency(summary.totalNotional, 'USDT'),
      meta: t('executionLive.summary.notionalMeta'),
      tone: 'info',
    },
    {
      id: 'slippage',
      value: formatBps(summary.avgSlippageBps),
      meta: t('executionLive.summary.slippageMeta'),
      tone: getSlippageTone(summary.avgSlippageBps),
    },
    {
      id: 'latency',
      value: `${formatNumber(summary.p95LatencyMs, 0)} ms`,
      meta: t('executionLive.summary.latencyMeta'),
      tone: connectionTone(connectionState),
    },
    {
      id: 'alerts',
      value: formatNumber((summary.rejected || 0) + (summary.slippageAlerts || 0), 0),
      meta: t('executionLive.summary.alertsMeta'),
      tone: ((summary.rejected || 0) + (summary.slippageAlerts || 0)) > 0 ? 'warn' : 'good',
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
    <div className="execution-page">
      <nav className="navbar-main execution-navbar">
        <div className="logo">
          <img src={logo} alt="Logo" className="logo-purple" />
          <span>{t('navbar.logo')}</span>
        </div>
        <div className="nav-links">
          <a href="/main">{t('navbar.trade')}</a>
          <a href="/profile">{t('navbar.profile')}</a>
          <a href="/venues">{t('navbar.venues')}</a>
          <a href="/hedgeflows">{t('navbar.hedgeflows')}</a>
          <a href="/hedge-pnl">{t('navbar.hedgePnl')}</a>
          <a href="/execution-live" className="active">{t('navbar.executionLive')}</a>
          <a href="/reconciliation-alerts">{t('navbar.reconciliationAlerts')}</a>
          <a href="/manual-override">{t('navbar.manualOverride')}</a>
          <a href="/policy-config">{t('navbar.policyConfig')}</a>
          <a href="/replay">{t('navbar.replay')}</a>
          <button onClick={handleLogout} className="logout-btn">{t('navbar.logout')}</button>
        </div>
      </nav>

      <main className="execution-shell">
        <section className="execution-hero">
          <div>
            <span className="execution-kicker">{t('executionLive.kicker')}</span>
            <h1>{t('executionLive.title')}</h1>
            <p>{t('executionLive.subtitle')}</p>
          </div>
          <div className="execution-hero-meta">
            <button type="button" className="execution-refresh-btn" onClick={() => loadSnapshot({ showLoader: true })}>
              {t('executionLive.actions.refresh')}
            </button>
            <button type="button" className="execution-refresh-btn ghost" onClick={() => setPaused((value) => !value)}>
              {paused ? t('executionLive.actions.resume') : t('executionLive.actions.pause')}
            </button>
            <div className={`execution-live-pill execution-live-${connectionTone(connectionState)}`}>
              <span className="execution-live-dot" />
              {t(`executionLive.connection.${connectionState}`, { defaultValue: connectionState })}
            </div>
            <div className="execution-refresh-label">
              {t('executionLive.lastEvent', {
                time: lastEventAt ? formatDateTime(lastEventAt) : '-',
              })}
            </div>
          </div>
        </section>

        <section className="execution-toolbar">
          <label>
            <span>{t('executionLive.filters.symbol')}</span>
            <select value={symbolFilter} onChange={(event) => setSymbolFilter(event.target.value)}>
              <option value="all">{t('executionLive.filters.all')}</option>
              {(feed.filters?.symbols || []).map((symbol) => (
                <option key={symbol} value={symbol}>{symbol}</option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('executionLive.filters.venue')}</span>
            <select value={venueFilter} onChange={(event) => setVenueFilter(event.target.value)}>
              <option value="all">{t('executionLive.filters.all')}</option>
              {(feed.filters?.venues || []).map((venue) => (
                <option key={venue} value={venue}>{venue}</option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('executionLive.filters.provider')}</span>
            <select value={providerFilter} onChange={(event) => setProviderFilter(event.target.value)}>
              <option value="all">{t('executionLive.filters.all')}</option>
              {(feed.filters?.providers || []).map((provider) => (
                <option key={provider} value={provider}>{provider}</option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('executionLive.filters.status')}</span>
            <select value={statusFilter} onChange={(event) => setStatusFilter(event.target.value)}>
              <option value="all">{t('executionLive.filters.all')}</option>
              {(feed.filters?.statuses || []).map((status) => (
                <option key={status} value={status}>{status}</option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('executionLive.filters.side')}</span>
            <select value={sideFilter} onChange={(event) => setSideFilter(event.target.value)}>
              <option value="all">{t('executionLive.filters.all')}</option>
              {(feed.filters?.sides || []).map((side) => (
                <option key={side} value={side}>{side}</option>
              ))}
            </select>
          </label>
          <label className="execution-search-field">
            <span>{t('executionLive.filters.search')}</span>
            <input
              value={searchFilter}
              onChange={(event) => setSearchFilter(event.target.value)}
              placeholder={t('executionLive.filters.searchPlaceholder')}
            />
          </label>
          <div className="execution-source-pill">{feed.source || 'api'}</div>
        </section>

        <section className="execution-summary-grid">
          {summaryCards.map((card) => (
            <article className={`execution-summary-card execution-value-${card.tone}`} key={card.id}>
              <span>{t(`executionLive.summary.${card.id}`)}</span>
              <strong>{card.value}</strong>
              <small>{card.meta}</small>
            </article>
          ))}
        </section>

        {error && <div className="execution-empty-state error">{error}</div>}

        <section className="execution-layout">
          <div className="execution-feed-panel">
            <div className="execution-panel-title">
              <div>
                <h2>{t('executionLive.feed.title')}</h2>
                <p>{t('executionLive.feed.subtitle', { count: events.length })}</p>
              </div>
              <span>{t('executionLive.feed.topic')}</span>
            </div>

            {isLoading ? (
              <div className="execution-empty-state">{t('executionLive.states.loading')}</div>
            ) : events.length === 0 ? (
              <div className="execution-empty-state">{t('executionLive.states.empty')}</div>
            ) : (
              <div className="execution-table">
                <div className="execution-table-head execution-row-layout">
                  <span>{t('executionLive.table.time')}</span>
                  <span>{t('executionLive.table.execution')}</span>
                  <span>{t('executionLive.table.route')}</span>
                  <span>{t('executionLive.table.fill')}</span>
                  <span>{t('executionLive.table.quality')}</span>
                  <span>{t('executionLive.table.status')}</span>
                </div>
                <div className="execution-table-body">
                  {events.map((row) => {
                    const key = eventKey(row);
                    const statusTone = getStatusTone(row.status);
                    return (
                      <button
                        type="button"
                        className={`execution-row execution-row-layout ${selectedEventKey === key ? 'selected' : ''}`}
                        key={key}
                        onClick={() => setSelectedEventKey(key)}
                      >
                        <span>
                          <strong>{shortTime(row.timestamp)}</strong>
                          <small>{formatNumber(row.latencyMs, 0)} ms</small>
                        </span>
                        <span>
                          <strong>{row.executionId}</strong>
                          <small>{row.hedgeFlowId}</small>
                        </span>
                        <span>
                          <strong>{row.venueId}</strong>
                          <small>{row.symbol} / {row.side}</small>
                        </span>
                        <span>
                          <strong>{formatQty(row.filledQty, row.symbol)}</strong>
                          <small>@ {formatPrice(row.avgFillPrice)}</small>
                        </span>
                        <span>
                          <strong className={`execution-value-${getSlippageTone(row.slippageBps)}`}>
                            {formatBps(row.slippageBps)}
                          </strong>
                          <small>{formatCurrency(-Math.abs(Number(row.fee || 0)), row.feeCurrency)}</small>
                        </span>
                        <span>
                          <b className={`execution-status execution-status-${statusTone}`}>{row.status}</b>
                          {row.live && <small className="execution-live-tag">{t('executionLive.feed.live')}</small>}
                        </span>
                      </button>
                    );
                  })}
                </div>
              </div>
            )}
          </div>

          <aside className="execution-inspector">
            {selectedEvent ? (
              <>
                <div className="execution-inspector-hero">
                  <span className="execution-kicker">{t('executionLive.inspector.kicker')}</span>
                  <h2>{selectedEvent.executionId}</h2>
                  <p>{selectedEvent.venueId} / {selectedEvent.symbol} / {selectedEvent.sourceTopic}</p>
                </div>

                <div className="execution-detail-grid">
                  <div>
                    <span>{t('executionLive.inspector.notional')}</span>
                    <strong>{formatCurrency(selectedEvent.notional, selectedEvent.feeCurrency)}</strong>
                  </div>
                  <div>
                    <span>{t('executionLive.inspector.pnl')}</span>
                    <strong className={`execution-value-${getPnlTone(selectedEvent.hedgePnl)}`}>
                      {formatCurrency(selectedEvent.hedgePnl, selectedEvent.feeCurrency)}
                    </strong>
                  </div>
                  <div>
                    <span>{t('executionLive.inspector.reference')}</span>
                    <strong>{formatPrice(selectedEvent.referenceMid)}</strong>
                  </div>
                  <div>
                    <span>{t('executionLive.inspector.received')}</span>
                    <strong>{shortTime(selectedEvent.receivedAt)}</strong>
                  </div>
                </div>

                <dl className="execution-facts">
                  <div>
                    <dt>{t('executionLive.inspector.batch')}</dt>
                    <dd>{selectedEvent.batchId}</dd>
                  </div>
                  <div>
                    <dt>{t('executionLive.inspector.provider')}</dt>
                    <dd>{selectedEvent.providerId}</dd>
                  </div>
                  <div>
                    <dt>{t('executionLive.inspector.childOrder')}</dt>
                    <dd>{selectedEvent.childOrderId || '-'}</dd>
                  </div>
                  <div>
                    <dt>{t('executionLive.inspector.clientOrder')}</dt>
                    <dd>{selectedEvent.clientOrderId || '-'}</dd>
                  </div>
                  <div>
                    <dt>{t('executionLive.inspector.routeType')}</dt>
                    <dd>{selectedEvent.routeType || '-'}</dd>
                  </div>
                  <div>
                    <dt>{t('executionLive.inspector.urgency')}</dt>
                    <dd>{selectedEvent.urgency || '-'}</dd>
                  </div>
                </dl>
              </>
            ) : (
              <div className="execution-empty-state compact">{t('executionLive.states.select')}</div>
            )}
          </aside>
        </section>
      </main>
    </div>
  );
};

export default ExecutionLiveFeed;
