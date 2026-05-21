import React, { useCallback, useDeferredValue, useEffect, useMemo, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { isAuthenticated, logout } from '../../api/authService';
import { getHedgeFlowById, getHedgeFlows } from '../../api/hedgeFlowService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import HedgeFlowDrilldown from './HedgeFlowDrilldown';
import HedgeFlowTable from './HedgeFlowTable';
import {
  STATUS_FILTERS,
  formatBps,
  formatCurrency,
  formatDateTime,
  formatPct,
} from './hedgeFlowUtils';
import './HedgeFlowMonitor.css';

const POLL_INTERVAL_MS = 5000;

const defaultResponse = {
  items: [],
  total: 0,
  summary: {},
  filters: {
    statuses: [],
    symbols: [],
    venues: [],
    providers: [],
  },
  generatedAt: null,
};

const HedgeFlowMonitor = () => {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const [isAuth, setIsAuth] = useState(null);
  const [flowsResponse, setFlowsResponse] = useState(defaultResponse);
  const [selectedFlowId, setSelectedFlowId] = useState('');
  const [selectedFlowDetail, setSelectedFlowDetail] = useState(null);
  const [listLoading, setListLoading] = useState(true);
  const [detailLoading, setDetailLoading] = useState(false);
  const [listError, setListError] = useState('');
  const [detailError, setDetailError] = useState('');
  const [statusFilter, setStatusFilter] = useState('all');
  const [symbolFilter, setSymbolFilter] = useState('all');
  const [venueFilter, setVenueFilter] = useState('all');
  const [providerFilter, setProviderFilter] = useState('all');
  const [search, setSearch] = useState('');

  const deferredSearch = useDeferredValue(search);

  useEffect(() => {
    const checkAuth = async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    };
    checkAuth();
  }, [navigate]);

  const loadFlows = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setListLoading(true);
    try {
      const response = await getHedgeFlows({
        status: statusFilter,
        symbol: symbolFilter,
        venue: venueFilter,
        providerId: providerFilter,
        search: deferredSearch,
      });
      setFlowsResponse(response);
      setListError('');
      setSelectedFlowId((current) => {
        if (response.items.some((flow) => flow.hedgeFlowId === current)) return current;
        return response.items[0]?.hedgeFlowId || '';
      });
    } catch (error) {
      setListError(t('hedgeFlow.states.error'));
    } finally {
      if (showLoader) setListLoading(false);
    }
  }, [deferredSearch, providerFilter, statusFilter, symbolFilter, t, venueFilter]);

  useEffect(() => {
    if (isAuth) {
      loadFlows({ showLoader: true });
    }
  }, [isAuth, loadFlows]);

  useInterval(() => {
    if (isAuth) {
      loadFlows();
    }
  }, isAuth ? POLL_INTERVAL_MS : null);

  useEffect(() => {
    let cancelled = false;

    const loadDetail = async () => {
      if (!selectedFlowId) {
        setSelectedFlowDetail(null);
        return;
      }

      setDetailLoading(true);
      try {
        const response = await getHedgeFlowById(selectedFlowId);
        if (!cancelled) {
          setSelectedFlowDetail(response);
          setDetailError('');
        }
      } catch (error) {
        if (!cancelled) {
          setSelectedFlowDetail(null);
          setDetailError(t('hedgeFlow.states.detailError'));
        }
      } finally {
        if (!cancelled) setDetailLoading(false);
      }
    };

    loadDetail();
    return () => {
      cancelled = true;
    };
  }, [selectedFlowId, t]);

  const selectedSummary = useMemo(
    () => flowsResponse.items.find((flow) => flow.hedgeFlowId === selectedFlowId) || null,
    [flowsResponse.items, selectedFlowId]
  );

  const selectedFlow = selectedFlowDetail || selectedSummary;
  const summary = flowsResponse.summary || {};

  const summaryCards = [
    {
      id: 'open',
      value: summary.open ?? 0,
      meta: t('hedgeFlow.summary.openMeta'),
    },
    {
      id: 'completion',
      value: formatPct(summary.completionPct),
      meta: t('hedgeFlow.summary.completionMeta'),
    },
    {
      id: 'pnl',
      value: formatCurrency(summary.hedgePnl, 'USDT'),
      meta: t('hedgeFlow.summary.pnlMeta'),
    },
    {
      id: 'slippage',
      value: formatBps(summary.avgSlippageBps),
      meta: t('hedgeFlow.summary.slippageMeta'),
    },
    {
      id: 'alerts',
      value: summary.alerts ?? 0,
      meta: t('hedgeFlow.summary.alertsMeta'),
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
    <div className="hedge-page">
      <nav className="navbar-main hedge-navbar">
        <div className="logo">
          <img src={logo} alt="Logo" className="logo-purple" />
          <span>{t('navbar.logo')}</span>
        </div>
        <div className="nav-links">
          <a href="/main">{t('navbar.trade')}</a>
          <a href="/profile">{t('navbar.profile')}</a>
          <a href="/venues">{t('navbar.venues')}</a>
          <a href="/hedgeflows" className="active">{t('navbar.hedgeflows')}</a>
          <a href="/hedge-pnl">{t('navbar.hedgePnl')}</a>
          <a href="/execution-live">{t('navbar.executionLive')}</a>
          <a href="/reconciliation-alerts">{t('navbar.reconciliationAlerts')}</a>
          <a href="/manual-override">{t('navbar.manualOverride')}</a>
          <a href="/policy-config">{t('navbar.policyConfig')}</a>
          <a href="/replay">{t('navbar.replay')}</a>
          <button onClick={handleLogout} className="logout-btn">{t('navbar.logout')}</button>
        </div>
      </nav>

      <main className="hedge-shell">
        <section className="hedge-hero">
          <div className="hedge-hero-copy">
            <span className="hedge-kicker">{t('hedgeFlow.kicker')}</span>
            <h1>{t('hedgeFlow.title')}</h1>
            <p>{t('hedgeFlow.subtitle')}</p>
          </div>
          <div className="hedge-hero-meta">
            <button type="button" className="hedge-refresh-btn" onClick={() => loadFlows({ showLoader: true })}>
              {t('hedgeFlow.actions.refresh')}
            </button>
            <div className="hedge-live-pill">
              <span className="hedge-live-dot" />
              {t('hedgeFlow.live', { seconds: POLL_INTERVAL_MS / 1000 })}
            </div>
            <div className="hedge-refresh-label">
              {t('hedgeFlow.lastUpdated', {
                time: flowsResponse.generatedAt ? formatDateTime(flowsResponse.generatedAt) : '-',
              })}
            </div>
          </div>
        </section>

        <section className="hedge-summary-grid">
          {summaryCards.map((card) => (
            <article className={`hedge-summary-card hedge-summary-${card.id}`} key={card.id}>
              <span>{t(`hedgeFlow.summary.${card.id}`)}</span>
              <strong>{card.value}</strong>
              <small>{card.meta}</small>
            </article>
          ))}
        </section>

        <section className="hedge-toolbar">
          <div className="hedge-status-filter" aria-label={t('hedgeFlow.filters.status')}>
            {STATUS_FILTERS.map((status) => (
              <button
                type="button"
                key={status}
                className={`hedge-filter-chip ${statusFilter === status ? 'active' : ''}`}
                onClick={() => setStatusFilter(status)}
              >
                {status === 'all'
                  ? t('hedgeFlow.filters.all')
                  : t(`hedgeFlow.status.${status}`, { defaultValue: status })}
              </button>
            ))}
          </div>

          <div className="hedge-field-group">
            <label>
              <span>{t('hedgeFlow.filters.symbol')}</span>
              <select value={symbolFilter} onChange={(event) => setSymbolFilter(event.target.value)}>
                <option value="all">{t('hedgeFlow.filters.all')}</option>
                {(flowsResponse.filters?.symbols || []).map((symbol) => (
                  <option key={symbol} value={symbol}>{symbol}</option>
                ))}
              </select>
            </label>
            <label>
              <span>{t('hedgeFlow.filters.venue')}</span>
              <select value={venueFilter} onChange={(event) => setVenueFilter(event.target.value)}>
                <option value="all">{t('hedgeFlow.filters.all')}</option>
                {(flowsResponse.filters?.venues || []).map((venue) => (
                  <option key={venue} value={venue}>{venue}</option>
                ))}
              </select>
            </label>
            <label>
              <span>{t('hedgeFlow.filters.provider')}</span>
              <select value={providerFilter} onChange={(event) => setProviderFilter(event.target.value)}>
                <option value="all">{t('hedgeFlow.filters.all')}</option>
                {(flowsResponse.filters?.providers || []).map((provider) => (
                  <option key={provider} value={provider}>{provider}</option>
                ))}
              </select>
            </label>
            <label className="hedge-search-field">
              <span>{t('hedgeFlow.filters.search')}</span>
              <input
                type="text"
                value={search}
                onChange={(event) => setSearch(event.target.value)}
                placeholder={t('hedgeFlow.filters.searchPlaceholder')}
              />
            </label>
          </div>
        </section>

        <section className="hedge-layout">
          <div className="hedge-list-panel">
            <div className="hedge-panel-title list-title">
              <div>
                <h2>{t('hedgeFlow.list.title')}</h2>
                <p>{t('hedgeFlow.list.subtitle', { count: flowsResponse.total || 0 })}</p>
              </div>
              <span>{flowsResponse.source || 'api'}</span>
            </div>

            {listLoading ? (
              <div className="hedge-empty-state">{t('hedgeFlow.states.loading')}</div>
            ) : listError ? (
              <div className="hedge-empty-state error">{listError}</div>
            ) : flowsResponse.items.length === 0 ? (
              <div className="hedge-empty-state">{t('hedgeFlow.states.empty')}</div>
            ) : (
              <HedgeFlowTable
                flows={flowsResponse.items}
                selectedFlowId={selectedFlowId}
                onSelect={setSelectedFlowId}
              />
            )}
          </div>

          {detailError ? (
            <aside className="hedge-drilldown">
              <div className="hedge-empty-state error">{detailError}</div>
            </aside>
          ) : (
            <HedgeFlowDrilldown flow={selectedFlow} isLoading={detailLoading} />
          )}
        </section>
      </main>
    </div>
  );
};

export default HedgeFlowMonitor;
