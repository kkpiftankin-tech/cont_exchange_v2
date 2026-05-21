import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import {
  Area,
  Bar,
  BarChart,
  CartesianGrid,
  ComposedChart,
  Legend,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';
import { useTranslation } from 'react-i18next';
import { isAuthenticated, logout } from '../../api/authService';
import { getHedgePnlDashboard } from '../../api/hedgeFlowService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import {
  formatBps,
  formatCurrency,
  formatDateTime,
  formatNumber,
  formatPrice,
  getPnlTone,
  getSlippageTone,
} from '../HedgeFlowMonitor/hedgeFlowUtils';
import './HedgePnlDashboard.css';

const POLL_INTERVAL_MS = 5000;

const emptyDashboard = {
  summary: {},
  timeSeries: [],
  symbolBreakdown: [],
  venueBreakdown: [],
  filters: {
    symbols: [],
    venues: [],
    providers: [],
  },
  generatedAt: null,
  source: '',
};

function shortTime(value) {
  if (!value) return '-';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

const ChartTooltip = ({ active, payload, label }) => {
  if (!active || !payload?.length) return null;
  return (
    <div className="pnl-chart-tooltip">
      <strong>{formatDateTime(label)}</strong>
      {payload.map((item) => (
        <span key={item.dataKey} style={{ color: item.color }}>
          {item.name}: {typeof item.value === 'number' ? formatNumber(item.value, 2) : item.value}
        </span>
      ))}
    </div>
  );
};

const HedgePnlDashboard = () => {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const [isAuth, setIsAuth] = useState(null);
  const [dashboard, setDashboard] = useState(emptyDashboard);
  const [isLoading, setIsLoading] = useState(true);
  const [error, setError] = useState('');
  const [symbolFilter, setSymbolFilter] = useState('all');
  const [venueFilter, setVenueFilter] = useState('all');
  const [providerFilter, setProviderFilter] = useState('all');

  useEffect(() => {
    const checkAuth = async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    };
    checkAuth();
  }, [navigate]);

  const loadDashboard = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setIsLoading(true);
    try {
      const response = await getHedgePnlDashboard({
        symbol: symbolFilter,
        venue: venueFilter,
        providerId: providerFilter,
      });
      setDashboard(response);
      setError('');
    } catch (err) {
      setError(t('hedgePnl.states.error'));
    } finally {
      if (showLoader) setIsLoading(false);
    }
  }, [providerFilter, symbolFilter, t, venueFilter]);

  useEffect(() => {
    if (isAuth) {
      loadDashboard({ showLoader: true });
    }
  }, [isAuth, loadDashboard]);

  useInterval(() => {
    if (isAuth) {
      loadDashboard();
    }
  }, isAuth ? POLL_INTERVAL_MS : null);

  const latestRows = useMemo(
    () => [...(dashboard.timeSeries || [])]
      .sort((left, right) => (Date.parse(right.timestamp) || 0) - (Date.parse(left.timestamp) || 0))
      .slice(0, 8),
    [dashboard.timeSeries]
  );

  const summary = dashboard.summary || {};
  const analyticsChips = [
    {
      id: 'notional',
      value: formatCurrency(summary.totalNotional, 'USDT'),
    },
    {
      id: 'filledQty',
      value: formatNumber(summary.totalFilledQty, 4),
    },
    {
      id: 'flows',
      value: `${formatNumber(summary.flowCount, 0)} / ${formatNumber(summary.reportCount, 0)}`,
    },
    {
      id: 'spread',
      value: formatPrice(summary.avgPriceSpread),
    },
  ];
  const summaryCards = [
    {
      id: 'netPnl',
      value: formatCurrency(summary.netAfterFees, 'USDT'),
      tone: getPnlTone(summary.netAfterFees),
      meta: t('hedgePnl.summary.netPnlMeta'),
    },
    {
      id: 'grossPnl',
      value: formatCurrency(summary.grossBeforeFees, 'USDT'),
      tone: getPnlTone(summary.grossBeforeFees),
      meta: t('hedgePnl.summary.grossPnlMeta'),
    },
    {
      id: 'fees',
      value: formatCurrency(-Math.abs(Number(summary.totalFees || 0)), 'USDT'),
      tone: 'warn',
      meta: t('hedgePnl.summary.feesMeta'),
    },
    {
      id: 'slippage',
      value: formatBps(summary.avgSlippageBps),
      tone: getSlippageTone(summary.avgSlippageBps),
      meta: t('hedgePnl.summary.slippageMeta'),
    },
    {
      id: 'winRate',
      value: `${formatNumber(summary.winRatePct, 1)}%`,
      tone: 'info',
      meta: t('hedgePnl.summary.winRateMeta', { count: summary.reportCount || 0 }),
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
    <div className="pnl-page">
      <nav className="navbar-main pnl-navbar">
        <div className="logo">
          <img src={logo} alt="Logo" className="logo-purple" />
          <span>{t('navbar.logo')}</span>
        </div>
        <div className="nav-links">
          <a href="/main">{t('navbar.trade')}</a>
          <a href="/profile">{t('navbar.profile')}</a>
          <a href="/venues">{t('navbar.venues')}</a>
          <a href="/hedgeflows">{t('navbar.hedgeflows')}</a>
          <a href="/hedge-pnl" className="active">{t('navbar.hedgePnl')}</a>
          <a href="/execution-live">{t('navbar.executionLive')}</a>
          <a href="/reconciliation-alerts">{t('navbar.reconciliationAlerts')}</a>
          <a href="/manual-override">{t('navbar.manualOverride')}</a>
          <a href="/policy-config">{t('navbar.policyConfig')}</a>
          <a href="/replay">{t('navbar.replay')}</a>
          <button onClick={handleLogout} className="logout-btn">{t('navbar.logout')}</button>
        </div>
      </nav>

      <main className="pnl-shell">
        <section className="pnl-hero">
          <div>
            <span className="pnl-kicker">{t('hedgePnl.kicker')}</span>
            <h1>{t('hedgePnl.title')}</h1>
            <p>{t('hedgePnl.subtitle')}</p>
          </div>
          <div className="pnl-hero-meta">
            <button type="button" className="pnl-refresh-btn" onClick={() => loadDashboard({ showLoader: true })}>
              {t('hedgePnl.actions.refresh')}
            </button>
            <div className="pnl-live-pill">
              <span className="pnl-live-dot" />
              {t('hedgePnl.live', { seconds: POLL_INTERVAL_MS / 1000 })}
            </div>
            <div className="pnl-refresh-label">
              {t('hedgePnl.lastUpdated', {
                time: dashboard.generatedAt ? formatDateTime(dashboard.generatedAt) : '-',
              })}
            </div>
          </div>
        </section>

        <section className="pnl-toolbar">
          <label>
            <span>{t('hedgePnl.filters.symbol')}</span>
            <select value={symbolFilter} onChange={(event) => setSymbolFilter(event.target.value)}>
              <option value="all">{t('hedgePnl.filters.all')}</option>
              {(dashboard.filters?.symbols || []).map((symbol) => (
                <option key={symbol} value={symbol}>{symbol}</option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('hedgePnl.filters.venue')}</span>
            <select value={venueFilter} onChange={(event) => setVenueFilter(event.target.value)}>
              <option value="all">{t('hedgePnl.filters.all')}</option>
              {(dashboard.filters?.venues || []).map((venue) => (
                <option key={venue} value={venue}>{venue}</option>
              ))}
            </select>
          </label>
          <label>
            <span>{t('hedgePnl.filters.provider')}</span>
            <select value={providerFilter} onChange={(event) => setProviderFilter(event.target.value)}>
              <option value="all">{t('hedgePnl.filters.all')}</option>
              {(dashboard.filters?.providers || []).map((provider) => (
                <option key={provider} value={provider}>{provider}</option>
              ))}
            </select>
          </label>
          <span className="pnl-source-pill">{dashboard.source || 'api'}</span>
        </section>

        <section className="pnl-analytics-strip">
          {analyticsChips.map((chip) => (
            <div className="pnl-analytics-chip" key={chip.id}>
              <span>{t(`hedgePnl.analytics.${chip.id}`)}</span>
              <strong>{chip.value}</strong>
            </div>
          ))}
        </section>

        <section className="pnl-summary-grid">
          {summaryCards.map((card) => (
            <article className="pnl-summary-card" key={card.id}>
              <span>{t(`hedgePnl.summary.${card.id}`)}</span>
              <strong className={`pnl-value-${card.tone}`}>{card.value}</strong>
              <small>{card.meta}</small>
            </article>
          ))}
        </section>

        {isLoading ? (
          <div className="pnl-empty-state">{t('hedgePnl.states.loading')}</div>
        ) : error ? (
          <div className="pnl-empty-state error">{error}</div>
        ) : dashboard.timeSeries.length === 0 ? (
          <div className="pnl-empty-state">{t('hedgePnl.states.empty')}</div>
        ) : (
          <>
            <section className="pnl-chart-layout">
              <article className="pnl-panel pnl-wide-panel">
                <div className="pnl-panel-title">
                  <div>
                    <h2>{t('hedgePnl.charts.pnlTitle')}</h2>
                    <p>{t('hedgePnl.charts.pnlSubtitle')}</p>
                  </div>
                </div>
                <div className="pnl-chart-box">
                  <ResponsiveContainer width="100%" height={320}>
                    <ComposedChart data={dashboard.timeSeries}>
                      <defs>
                        <linearGradient id="pnlGradient" x1="0" x2="0" y1="0" y2="1">
                          <stop offset="0%" stopColor="#a74aff" stopOpacity={0.55} />
                          <stop offset="100%" stopColor="#a74aff" stopOpacity={0.02} />
                        </linearGradient>
                      </defs>
                      <CartesianGrid stroke="rgba(167, 74, 255, 0.14)" vertical={false} />
                      <XAxis dataKey="timestamp" tickFormatter={shortTime} stroke="#a799bf" />
                      <YAxis stroke="#a799bf" />
                      <Tooltip content={<ChartTooltip />} />
                      <Legend />
                      <Area
                        type="monotone"
                        name={t('hedgePnl.charts.cumulativePnl')}
                        dataKey="cumulativePnl"
                        stroke="#a74aff"
                        fill="url(#pnlGradient)"
                        strokeWidth={2}
                      />
                      <Line
                        type="monotone"
                        name={t('hedgePnl.charts.executionPnl')}
                        dataKey="hedgePnl"
                        stroke="#ffb36b"
                        strokeWidth={2}
                        dot={{ r: 3 }}
                      />
                    </ComposedChart>
                  </ResponsiveContainer>
                </div>
              </article>

              <article className="pnl-panel">
                <div className="pnl-panel-title">
                  <div>
                    <h2>{t('hedgePnl.charts.priceTitle')}</h2>
                    <p>{t('hedgePnl.charts.priceSubtitle')}</p>
                  </div>
                </div>
                <div className="pnl-chart-box">
                  <ResponsiveContainer width="100%" height={320}>
                    <LineChart data={dashboard.timeSeries}>
                      <CartesianGrid stroke="rgba(167, 74, 255, 0.14)" vertical={false} />
                      <XAxis dataKey="timestamp" tickFormatter={shortTime} stroke="#a799bf" />
                      <YAxis stroke="#a799bf" domain={['dataMin - 50', 'dataMax + 50']} />
                      <Tooltip content={<ChartTooltip />} />
                      <Legend />
                      <Line
                        type="monotone"
                        name={t('hedgePnl.charts.clearingPrice')}
                        dataKey="clearingPrice"
                        stroke="#d6c2ff"
                        strokeWidth={2}
                        dot={{ r: 3 }}
                      />
                      <Line
                        type="monotone"
                        name={t('hedgePnl.charts.avgFillPrice')}
                        dataKey="avgFillPrice"
                        stroke="#ffb36b"
                        strokeWidth={2}
                        dot={{ r: 3 }}
                      />
                    </LineChart>
                  </ResponsiveContainer>
                </div>
              </article>
            </section>

            <section className="pnl-breakdown-grid">
              <article className="pnl-panel">
                <div className="pnl-panel-title">
                  <h2>{t('hedgePnl.breakdown.symbolTitle')}</h2>
                </div>
                <ResponsiveContainer width="100%" height={260}>
                  <BarChart data={dashboard.symbolBreakdown}>
                    <CartesianGrid stroke="rgba(167, 74, 255, 0.14)" vertical={false} />
                    <XAxis dataKey="id" stroke="#a799bf" />
                    <YAxis stroke="#a799bf" />
                    <Tooltip />
                    <Bar dataKey="hedgePnl" name="PnL" fill="#a74aff" radius={[8, 8, 0, 0]} />
                  </BarChart>
                </ResponsiveContainer>
              </article>

              <article className="pnl-panel">
                <div className="pnl-panel-title">
                  <h2>{t('hedgePnl.breakdown.venueTitle')}</h2>
                </div>
                <ResponsiveContainer width="100%" height={260}>
                  <BarChart data={dashboard.venueBreakdown}>
                    <CartesianGrid stroke="rgba(167, 74, 255, 0.14)" vertical={false} />
                    <XAxis dataKey="id" stroke="#a799bf" />
                    <YAxis stroke="#a799bf" />
                    <Tooltip />
                    <Bar dataKey="hedgePnl" name="PnL" fill="#ffb36b" radius={[8, 8, 0, 0]} />
                  </BarChart>
                </ResponsiveContainer>
              </article>
            </section>

            <section className="pnl-panel">
              <div className="pnl-panel-title">
                <div>
                  <h2>{t('hedgePnl.table.title')}</h2>
                  <p>{t('hedgePnl.table.subtitle')}</p>
                </div>
              </div>
              <div className="pnl-execution-table">
                <div className="pnl-execution-head">
                  <span>{t('hedgePnl.table.time')}</span>
                  <span>{t('hedgePnl.table.context')}</span>
                  <span>{t('hedgePnl.table.prices')}</span>
                  <span>{t('hedgePnl.table.pnl')}</span>
                  <span>{t('hedgePnl.table.slippage')}</span>
                  <span>{t('hedgePnl.table.fee')}</span>
                </div>
                {latestRows.map((row) => (
                  <div className="pnl-execution-row" key={`${row.executionId}-${row.timestamp}`}>
                    <span>{formatDateTime(row.timestamp)}</span>
                    <span>
                      <strong>{row.symbol} · {row.venueId}</strong>
                      <small>{row.hedgeFlowId}</small>
                    </span>
                    <span>
                      <strong>{formatPrice(row.clearingPrice)} / {formatPrice(row.avgFillPrice)}</strong>
                      <small>{t('hedgePnl.table.clearingVsFill')}</small>
                    </span>
                    <span className={`pnl-value-${getPnlTone(row.hedgePnl)}`}>
                      {formatCurrency(row.hedgePnl, row.feeCurrency)}
                    </span>
                    <span className={`pnl-value-${getSlippageTone(row.slippageBps)}`}>
                      {formatBps(row.slippageBps)}
                    </span>
                    <span>{formatCurrency(-Math.abs(Number(row.fee || 0)), row.feeCurrency)}</span>
                  </div>
                ))}
              </div>
            </section>
          </>
        )}
      </main>
    </div>
  );
};

export default HedgePnlDashboard;
