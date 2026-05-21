import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { useNavigate, useSearchParams } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { isAuthenticated, logout } from '../../api/authService';
import { createManualOverride, getManualOverrideContext } from '../../api/hedgeFlowService';
import logo from '../../assets/logo-purple.svg';
import {
  formatBps,
  formatDateTime,
  formatNumber,
  formatPct,
  formatQty,
  getStatusTone,
} from '../HedgeFlowMonitor/hedgeFlowUtils';
import './ManualOverride.css';

const DEFAULT_FORM = {
  sourceAlertId: '',
  symbol: '',
  side: 'SELL',
  targetQty: '',
  venueIds: [],
  urgency: 'MEDIUM',
  priceConstraint: '',
  timeoutMs: 120000,
  providerId: 'operator-manual',
  reason: '',
};

const SLIPPAGE_LIMITS = {
  LOW: 8,
  MEDIUM: 15,
  HIGH: 25,
};

function formatNotional(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return '-';
  return `${formatNumber(number, 2)} USDT`;
}

function intentKey(intent) {
  return intent.intentId || `${intent.symbol}-${intent.createdAt}`;
}

function manualStatusTone(status) {
  const normalized = String(status || '').toUpperCase();
  if (normalized === 'ACCEPTED') return 'good';
  if (normalized === 'RISK_REJECTED') return 'bad';
  return getStatusTone(status);
}

function buildFormFromAlert(alert, currentForm = DEFAULT_FORM) {
  if (!alert) return currentForm;
  return {
    ...currentForm,
    sourceAlertId: alert.alertId,
    symbol: alert.symbol || currentForm.symbol,
    side: alert.side || currentForm.side,
    targetQty: alert.gapQty ? String(alert.gapQty) : currentForm.targetQty,
    venueIds: alert.venueIds?.length ? alert.venueIds : currentForm.venueIds,
    urgency: alert.urgency || currentForm.urgency,
    timeoutMs: alert.timeoutMs || currentForm.timeoutMs,
    providerId: alert.providerId || currentForm.providerId,
    priceConstraint: alert.referenceMid ? String(alert.referenceMid) : currentForm.priceConstraint,
    reason: alert.nextAction || alert.statusReason || currentForm.reason,
  };
}

const ManualOverride = () => {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const [searchParams] = useSearchParams();
  const requestedSourceAlertId = searchParams.get('sourceAlertId') || '';

  const [isAuth, setIsAuth] = useState(null);
  const [context, setContext] = useState(null);
  const [form, setForm] = useState(DEFAULT_FORM);
  const [isLoading, setIsLoading] = useState(true);
  const [isSubmitting, setIsSubmitting] = useState(false);
  const [error, setError] = useState('');
  const [message, setMessage] = useState('');
  const [createdIntent, setCreatedIntent] = useState(null);
  const [sourceApplied, setSourceApplied] = useState(false);

  useEffect(() => {
    const checkAuth = async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    };
    checkAuth();
  }, [navigate]);

  const loadContext = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setIsLoading(true);
    try {
      const response = await getManualOverrideContext();
      setContext(response);
      setError('');
      setForm((current) => ({
        ...current,
        symbol: current.symbol || response.options.symbols[0] || '',
        venueIds: current.venueIds.length ? current.venueIds : [response.options.venues[0]].filter(Boolean),
        providerId: current.providerId || response.defaults.providerId || 'operator-manual',
        timeoutMs: current.timeoutMs || response.defaults.timeoutMs || 120000,
      }));
    } catch (err) {
      setError(t('manualOverride.states.error'));
    } finally {
      if (showLoader) setIsLoading(false);
    }
  }, [t]);

  useEffect(() => {
    if (isAuth) {
      loadContext({ showLoader: true });
    }
  }, [isAuth, loadContext]);

  useEffect(() => {
    if (!context || sourceApplied || !requestedSourceAlertId) return;
    const alert = context.sourceAlerts.find((item) => item.alertId === requestedSourceAlertId);
    if (alert) {
      setForm((current) => buildFormFromAlert(alert, current));
      setSourceApplied(true);
    }
  }, [context, requestedSourceAlertId, sourceApplied]);

  const selectedAlert = useMemo(
    () => context?.sourceAlerts.find((alert) => alert.alertId === form.sourceAlertId) || null,
    [context, form.sourceAlertId]
  );

  const referenceMid = Number(form.priceConstraint || selectedAlert?.referenceMid || 0);
  const targetQty = Number(form.targetQty || 0);
  const targetNotional = Number.isFinite(referenceMid) && Number.isFinite(targetQty)
    ? referenceMid * targetQty
    : 0;
  const maxSlippage = SLIPPAGE_LIMITS[form.urgency] ?? SLIPPAGE_LIMITS.MEDIUM;
  const riskTone = targetNotional > 250000 ? 'bad' : targetNotional > 100000 ? 'warn' : 'good';

  const summary = context?.summary || {};
  const summaryCards = [
    { id: 'alerts', value: formatNumber(summary.sourceAlerts, 0), tone: (summary.sourceAlerts || 0) > 0 ? 'warn' : 'good' },
    { id: 'recent', value: formatNumber(summary.recent, 0), tone: 'info' },
    { id: 'accepted', value: formatNumber(summary.accepted, 0), tone: 'good' },
    { id: 'rejected', value: formatNumber(summary.rejected, 0), tone: (summary.rejected || 0) > 0 ? 'bad' : 'good' },
  ];

  const updateField = (field, value) => {
    setForm((current) => ({
      ...current,
      [field]: value,
    }));
  };

  const handleSourceAlertChange = (value) => {
    const alert = context?.sourceAlerts.find((item) => item.alertId === value);
    setForm((current) => buildFormFromAlert(alert, { ...current, sourceAlertId: value }));
  };

  const handleVenueToggle = (venueId) => {
    setForm((current) => {
      const nextVenues = current.venueIds.includes(venueId)
        ? current.venueIds.filter((item) => item !== venueId)
        : [...current.venueIds, venueId];
      return {
        ...current,
        venueIds: nextVenues,
      };
    });
  };

  const handleSubmit = async (event) => {
    event.preventDefault();
    setIsSubmitting(true);
    setError('');
    setMessage('');
    try {
      const response = await createManualOverride({
        ...form,
        targetQty: Number(form.targetQty),
        priceConstraint: form.priceConstraint === '' ? null : Number(form.priceConstraint),
        timeoutMs: Number(form.timeoutMs),
      });
      setCreatedIntent(response.intent);
      setContext(response.context);
      setMessage(t('manualOverride.states.created', { intentId: response.intent.intentId }));
    } catch (err) {
      const errors = err?.response?.data?.errors || [];
      const details = errors.map((item) => item.message).join(', ');
      setError(details || t('manualOverride.states.submitError'));
    } finally {
      setIsSubmitting(false);
    }
  };

  const handleLogout = () => {
    logout();
    navigate('/login');
  };

  if (isAuth === null) {
    return <div className="loading-screen">{t('auth.loading')}</div>;
  }

  return (
    <div className="manual-page">
      <nav className="navbar-main manual-navbar">
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
          <a href="/execution-live">{t('navbar.executionLive')}</a>
          <a href="/reconciliation-alerts">{t('navbar.reconciliationAlerts')}</a>
          <a href="/manual-override" className="active">{t('navbar.manualOverride')}</a>
          <a href="/policy-config">{t('navbar.policyConfig')}</a>
          <a href="/replay">{t('navbar.replay')}</a>
          <button onClick={handleLogout} className="logout-btn">{t('navbar.logout')}</button>
        </div>
      </nav>

      <main className="manual-shell">
        <section className="manual-hero">
          <div>
            <span className="manual-kicker">{t('manualOverride.kicker')}</span>
            <h1>{t('manualOverride.title')}</h1>
            <p>{t('manualOverride.subtitle')}</p>
          </div>
          <div className="manual-hero-meta">
            <button type="button" className="manual-refresh-btn" onClick={() => loadContext({ showLoader: true })}>
              {t('manualOverride.actions.refresh')}
            </button>
            <div className="manual-source-pill">{context?.source || 'api'}</div>
            <div className="manual-refresh-label">
              {t('manualOverride.lastUpdated', {
                time: context?.generatedAt ? formatDateTime(context.generatedAt) : '-',
              })}
            </div>
          </div>
        </section>

        <section className="manual-summary-grid">
          {summaryCards.map((card) => (
            <article className={`manual-summary-card manual-value-${card.tone}`} key={card.id}>
              <span>{t(`manualOverride.summary.${card.id}`)}</span>
              <strong>{card.value}</strong>
              <small>{t(`manualOverride.summary.${card.id}Meta`)}</small>
            </article>
          ))}
        </section>

        {error && <div className="manual-empty-state error">{error}</div>}
        {message && <div className="manual-empty-state success">{message}</div>}

        <section className="manual-layout">
          <form className="manual-form-panel" onSubmit={handleSubmit}>
            <div className="manual-panel-title">
              <div>
                <h2>{t('manualOverride.form.title')}</h2>
                <p>{t('manualOverride.form.subtitle')}</p>
              </div>
              <span>execution.intents</span>
            </div>

            {isLoading ? (
              <div className="manual-empty-state">{t('manualOverride.states.loading')}</div>
            ) : (
              <>
                <div className="manual-form-grid">
                  <label className="manual-wide">
                    <span>{t('manualOverride.form.sourceAlert')}</span>
                    <select value={form.sourceAlertId} onChange={(event) => handleSourceAlertChange(event.target.value)}>
                      <option value="">{t('manualOverride.form.noSourceAlert')}</option>
                      {(context?.sourceAlerts || []).map((alert) => (
                        <option key={alert.alertId} value={alert.alertId}>
                          {alert.type} / {alert.hedgeFlowId} / {formatQty(alert.gapQty, alert.symbol)}
                        </option>
                      ))}
                    </select>
                  </label>

                  <label>
                    <span>{t('manualOverride.form.symbol')}</span>
                    <select value={form.symbol} onChange={(event) => updateField('symbol', event.target.value)} required>
                      {(context?.options.symbols || []).map((symbol) => (
                        <option key={symbol} value={symbol}>{symbol}</option>
                      ))}
                    </select>
                  </label>

                  <label>
                    <span>{t('manualOverride.form.side')}</span>
                    <select value={form.side} onChange={(event) => updateField('side', event.target.value)} required>
                      {(context?.options.sides || ['BUY', 'SELL']).map((side) => (
                        <option key={side} value={side}>{side}</option>
                      ))}
                    </select>
                  </label>

                  <label>
                    <span>{t('manualOverride.form.targetQty')}</span>
                    <input
                      type="number"
                      min="0"
                      step="0.0001"
                      value={form.targetQty}
                      onChange={(event) => updateField('targetQty', event.target.value)}
                      required
                    />
                  </label>

                  <label>
                    <span>{t('manualOverride.form.provider')}</span>
                    <select value={form.providerId} onChange={(event) => updateField('providerId', event.target.value)} required>
                      <option value="operator-manual">operator-manual</option>
                      {(context?.options.providers || []).map((provider) => (
                        <option key={provider} value={provider}>{provider}</option>
                      ))}
                    </select>
                  </label>

                  <label>
                    <span>{t('manualOverride.form.priceConstraint')}</span>
                    <input
                      type="number"
                      min="0"
                      step="0.01"
                      value={form.priceConstraint}
                      onChange={(event) => updateField('priceConstraint', event.target.value)}
                    />
                  </label>

                  <label>
                    <span>{t('manualOverride.form.timeoutMs')}</span>
                    <input
                      type="number"
                      min="100"
                      step="100"
                      value={form.timeoutMs}
                      onChange={(event) => updateField('timeoutMs', event.target.value)}
                      required
                    />
                  </label>
                </div>

                <div className="manual-section">
                  <span>{t('manualOverride.form.urgency')}</span>
                  <div className="manual-segmented">
                    {(context?.options.urgencies || ['LOW', 'MEDIUM', 'HIGH']).map((urgency) => (
                      <button
                        type="button"
                        key={urgency}
                        className={form.urgency === urgency ? 'active' : ''}
                        onClick={() => updateField('urgency', urgency)}
                      >
                        {urgency}
                      </button>
                    ))}
                  </div>
                </div>

                <div className="manual-section">
                  <span>{t('manualOverride.form.venues')}</span>
                  <div className="manual-venue-grid">
                    {(context?.options.venues || []).map((venueId) => (
                      <label className="manual-venue-choice" key={venueId}>
                        <input
                          type="checkbox"
                          checked={form.venueIds.includes(venueId)}
                          onChange={() => handleVenueToggle(venueId)}
                        />
                        <span>{venueId}</span>
                      </label>
                    ))}
                  </div>
                </div>

                <label className="manual-reason">
                  <span>{t('manualOverride.form.reason')}</span>
                  <textarea
                    value={form.reason}
                    onChange={(event) => updateField('reason', event.target.value)}
                    rows={4}
                    required
                  />
                </label>

                <section className="manual-preview-grid">
                  <div className={`manual-preview-card manual-value-${riskTone}`}>
                    <span>{t('manualOverride.preview.notional')}</span>
                    <strong>{formatNotional(targetNotional)}</strong>
                    <small>{t('manualOverride.preview.riskLimit')}</small>
                  </div>
                  <div className="manual-preview-card manual-value-info">
                    <span>{t('manualOverride.preview.maxSlippage')}</span>
                    <strong>{formatBps(maxSlippage)}</strong>
                    <small>{t('manualOverride.preview.urgency', { urgency: form.urgency })}</small>
                  </div>
                  <div className="manual-preview-card manual-value-info">
                    <span>{t('manualOverride.preview.route')}</span>
                    <strong>{form.venueIds.join(', ') || '-'}</strong>
                    <small>{t('manualOverride.preview.routeMeta')}</small>
                  </div>
                </section>

                <button
                  type="submit"
                  className="manual-submit-btn"
                  disabled={isSubmitting || form.venueIds.length === 0}
                >
                  {isSubmitting ? t('manualOverride.form.submitting') : t('manualOverride.form.submit')}
                </button>
              </>
            )}
          </form>

          <aside className="manual-side-panel">
            <div className="manual-panel-title compact">
              <div>
                <h2>{t('manualOverride.side.title')}</h2>
                <p>{t('manualOverride.side.subtitle')}</p>
              </div>
            </div>

            {selectedAlert && (
              <div className="manual-source-card">
                <span className="manual-kicker">{t('manualOverride.side.source')}</span>
                <h3>{selectedAlert.hedgeFlowId}</h3>
                <p>{selectedAlert.type} / {selectedAlert.sourceTopic}</p>
                <div className="manual-source-facts">
                  <div>
                    <span>{t('manualOverride.side.gap')}</span>
                    <strong>{formatQty(selectedAlert.gapQty, selectedAlert.symbol)}</strong>
                  </div>
                  <div>
                    <span>{t('manualOverride.side.gapPct')}</span>
                    <strong>{formatPct(selectedAlert.gapPct)}</strong>
                  </div>
                </div>
              </div>
            )}

            {createdIntent && (
              <div className="manual-created-card">
                <span className="manual-kicker">{t('manualOverride.side.created')}</span>
                <h3>{createdIntent.intentId}</h3>
                <b className={`manual-status manual-status-${manualStatusTone(createdIntent.status)}`}>
                  {createdIntent.status}
                </b>
                <p>{createdIntent.riskReason}</p>
              </div>
            )}

            <div className="manual-recent-list">
              <h3>{t('manualOverride.side.recent')}</h3>
              {(context?.recent || []).length === 0 ? (
                <div className="manual-empty-state compact">{t('manualOverride.states.noRecent')}</div>
              ) : (
                (context?.recent || []).map((intent) => (
                  <div className="manual-recent-row" key={intentKey(intent)}>
                    <span>
                      <strong>{intent.intentId}</strong>
                      <small>{formatDateTime(intent.createdAt)}</small>
                    </span>
                    <span>
                      <strong>{formatQty(intent.targetQty, intent.symbol)}</strong>
                      <small>{intent.venueIds.join(', ')}</small>
                    </span>
                    <b className={`manual-status manual-status-${manualStatusTone(intent.status)}`}>{intent.status}</b>
                  </div>
                ))
              )}
            </div>
          </aside>
        </section>
      </main>
    </div>
  );
};

export default ManualOverride;
