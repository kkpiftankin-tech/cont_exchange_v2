import React, { useCallback, useEffect, useMemo, useState } from 'react';
import NavBar from "../../components/NavBar";
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { isAuthenticated, logout } from '../../api/authService';
import { getPolicyConfig, updatePolicyConfig } from '../../api/hedgeFlowService';
import logo from '../../assets/logo-purple.svg';
import {
  formatBps,
  formatDateTime,
  formatNumber,
  formatPct,
  formatQty,
} from '../HedgeFlowMonitor/hedgeFlowUtils';
import './PolicyConfig.css';

const DEFAULT_URGENCIES = ['LOW', 'MEDIUM', 'HIGH'];
const DEFAULT_ORDER_TYPES = ['POST_ONLY', 'LIMIT', 'IOC', 'MARKET'];

function cloneConfig(config = {}) {
  return JSON.parse(JSON.stringify(config || {}));
}

function numberInputValue(value) {
  if (value === undefined || value === null) return '';
  return String(value);
}

function impactTone(item) {
  if (item.eligible && item.urgency === 'HIGH') return 'bad';
  if (item.eligible) return 'warn';
  return 'good';
}

const PolicyConfig = () => {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const [isAuth, setIsAuth] = useState(null);
  const [context, setContext] = useState(null);
  const [draft, setDraft] = useState(null);
  const [reason, setReason] = useState('');
  const [isLoading, setIsLoading] = useState(true);
  const [isSaving, setIsSaving] = useState(false);
  const [error, setError] = useState('');
  const [message, setMessage] = useState('');

  useEffect(() => {
    const checkAuth = async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    };
    checkAuth();
  }, [navigate]);

  const loadPolicy = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setIsLoading(true);
    try {
      const response = await getPolicyConfig();
      setContext(response);
      setDraft(cloneConfig(response.config));
      setError('');
    } catch (err) {
      setError(t('policyConfig.states.error'));
    } finally {
      if (showLoader) setIsLoading(false);
    }
  }, [t]);

  useEffect(() => {
    if (isAuth) {
      loadPolicy({ showLoader: true });
    }
  }, [isAuth, loadPolicy]);

  const urgencies = context?.options.urgencies?.length ? context.options.urgencies : DEFAULT_URGENCIES;
  const orderTypes = context?.options.orderTypes?.length ? context.options.orderTypes : DEFAULT_ORDER_TYPES;
  const symbols = useMemo(() => (
    context?.options.symbols?.length
      ? context.options.symbols
      : Object.keys(draft?.hedgeTriggerThreshold || {})
  ), [context, draft]);
  const summary = context?.summary || {};

  const summaryCards = [
    { id: 'revision', value: formatNumber(summary.revision, 0), tone: 'info' },
    { id: 'eligible', value: formatNumber(summary.eligibleFlows, 0), tone: (summary.eligibleFlows || 0) > 0 ? 'warn' : 'good' },
    { id: 'slippage', value: formatBps(summary.avgMaxSlippageBps), tone: (summary.avgMaxSlippageBps || 0) > 20 ? 'bad' : 'info' },
    { id: 'exposure', value: `${formatNumber(summary.hedgeExposureLimit, 0)} USDT`, tone: 'info' },
  ];

  const setDraftField = (field, value) => {
    setDraft((current) => ({
      ...current,
      [field]: value,
    }));
  };

  const updateThreshold = (symbol, value) => {
    setDraft((current) => ({
      ...current,
      hedgeTriggerThreshold: {
        ...(current.hedgeTriggerThreshold || {}),
        [symbol]: value === '' ? '' : Number(value),
      },
    }));
  };

  const updateUrgencyPolicy = (urgency, field, value) => {
    setDraft((current) => ({
      ...current,
      hedgeUrgencyPolicy: {
        ...(current.hedgeUrgencyPolicy || {}),
        [urgency]: {
          ...(current.hedgeUrgencyPolicy?.[urgency] || {}),
          [field]: field === 'orderType' ? value : Number(value),
        },
      },
    }));
  };

  const updateSlippage = (urgency, value) => {
    setDraft((current) => ({
      ...current,
      maxSlippageBps: {
        ...(current.maxSlippageBps || {}),
        [urgency]: value === '' ? '' : Number(value),
      },
    }));
  };

  const updateRiskLimit = (field, value) => {
    setDraft((current) => ({
      ...current,
      riskLimits: {
        ...(current.riskLimits || {}),
        [field]: value === '' ? '' : Number(value),
      },
    }));
  };

  const handleReset = () => {
    setDraft(cloneConfig(context?.config));
    setReason('');
    setError('');
    setMessage('');
  };

  const handleSubmit = async (event) => {
    event.preventDefault();
    setIsSaving(true);
    setError('');
    setMessage('');
    try {
      const response = await updatePolicyConfig({
        reason,
        updatedBy: 'operator-ui',
        config: draft,
      });
      setContext(response);
      setDraft(cloneConfig(response.config));
      setReason('');
      setMessage(t('policyConfig.states.saved', { revision: response.config.revision }));
    } catch (err) {
      const errors = err?.response?.data?.errors || [];
      const details = errors.map((item) => item.message).join(', ');
      setError(details || t('policyConfig.states.saveError'));
    } finally {
      setIsSaving(false);
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
    <div className="policy-page">
      <NavBar />

      <main className="policy-shell">
        <section className="policy-hero">
          <div>
            <span className="policy-kicker">{t('policyConfig.kicker')}</span>
            <h1>{t('policyConfig.title')}</h1>
            <p>{t('policyConfig.subtitle')}</p>
          </div>
          <div className="policy-hero-meta">
            <button type="button" className="policy-refresh-btn" onClick={() => loadPolicy({ showLoader: true })}>
              {t('policyConfig.actions.refresh')}
            </button>
            <div className="policy-source-pill">{context?.source || 'api'}</div>
            <div className="policy-refresh-label">
              {t('policyConfig.lastUpdated', {
                time: context?.generatedAt ? formatDateTime(context.generatedAt) : '-',
              })}
            </div>
          </div>
        </section>

        <section className="policy-summary-grid">
          {summaryCards.map((card) => (
            <article className={`policy-summary-card policy-value-${card.tone}`} key={card.id}>
              <span>{t(`policyConfig.summary.${card.id}`)}</span>
              <strong>{card.value}</strong>
              <small>{t(`policyConfig.summary.${card.id}Meta`)}</small>
            </article>
          ))}
        </section>

        {error && <div className="policy-empty-state error">{error}</div>}
        {message && <div className="policy-empty-state success">{message}</div>}

        <section className="policy-layout">
          <form className="policy-form-panel" onSubmit={handleSubmit} aria-label={t('policyConfig.form.title')}>
            <div className="policy-panel-title">
              <div>
                <h2>{t('policyConfig.form.title')}</h2>
                <p>{t('policyConfig.form.subtitle')}</p>
              </div>
              <span>{draft?.solverConfigId || 'solverconfig'}</span>
            </div>

            {isLoading || !draft ? (
              <div className="policy-empty-state">{t('policyConfig.states.loading')}</div>
            ) : (
              <>
                <section className="policy-config-section">
                  <div className="policy-section-title">
                    <h3>{t('policyConfig.form.general')}</h3>
                    <p>{t('policyConfig.form.generalMeta')}</p>
                  </div>
                  <div className="policy-form-grid">
                    <label>
                      <span>{t('policyConfig.form.solverConfigId')}</span>
                      <input
                        value={draft.solverConfigId || ''}
                        onChange={(event) => setDraftField('solverConfigId', event.target.value)}
                        required
                      />
                    </label>
                    <label>
                      <span>{t('policyConfig.form.hedgeExposureLimit')}</span>
                      <input
                        type="number"
                        min="0"
                        step="1000"
                        value={numberInputValue(draft.riskLimits?.hedgeExposureLimit)}
                        onChange={(event) => updateRiskLimit('hedgeExposureLimit', event.target.value)}
                        required
                      />
                    </label>
                    <label>
                      <span>{t('policyConfig.form.maxNotionalPerHedge')}</span>
                      <input
                        type="number"
                        min="0"
                        step="1000"
                        value={numberInputValue(draft.riskLimits?.maxNotionalPerHedge)}
                        onChange={(event) => updateRiskLimit('maxNotionalPerHedge', event.target.value)}
                        required
                      />
                    </label>
                  </div>
                </section>

                <section className="policy-config-section">
                  <div className="policy-section-title">
                    <h3>{t('policyConfig.form.thresholds')}</h3>
                    <p>{t('policyConfig.form.thresholdsMeta')}</p>
                  </div>
                  <div className="policy-threshold-grid">
                    {symbols.map((symbol) => (
                      <label key={symbol}>
                        <span>{symbol}</span>
                        <input
                          type="number"
                          min="0"
                          step="0.0001"
                          value={numberInputValue(draft.hedgeTriggerThreshold?.[symbol])}
                          onChange={(event) => updateThreshold(symbol, event.target.value)}
                          required
                        />
                      </label>
                    ))}
                  </div>
                </section>

                <section className="policy-config-section">
                  <div className="policy-section-title">
                    <h3>{t('policyConfig.form.urgency')}</h3>
                    <p>{t('policyConfig.form.urgencyMeta')}</p>
                  </div>
                  <div className="policy-urgency-table">
                    <div className="policy-urgency-head">
                      <span>{t('policyConfig.table.urgency')}</span>
                      <span>{t('policyConfig.table.minGap')}</span>
                      <span>{t('policyConfig.table.maxSlippage')}</span>
                      <span>{t('policyConfig.table.timeout')}</span>
                      <span>{t('policyConfig.table.orderType')}</span>
                    </div>
                    {urgencies.map((urgency) => (
                      <div className="policy-urgency-row" key={urgency}>
                        <strong>{urgency}</strong>
                        <input
                          type="number"
                          min="0"
                          step="0.1"
                          value={numberInputValue(draft.hedgeUrgencyPolicy?.[urgency]?.minGapPct)}
                          onChange={(event) => updateUrgencyPolicy(urgency, 'minGapPct', event.target.value)}
                        />
                        <input
                          type="number"
                          min="0"
                          step="0.1"
                          value={numberInputValue(draft.maxSlippageBps?.[urgency])}
                          onChange={(event) => updateSlippage(urgency, event.target.value)}
                        />
                        <input
                          type="number"
                          min="100"
                          step="100"
                          value={numberInputValue(draft.hedgeUrgencyPolicy?.[urgency]?.timeoutMs)}
                          onChange={(event) => updateUrgencyPolicy(urgency, 'timeoutMs', event.target.value)}
                        />
                        <select
                          value={draft.hedgeUrgencyPolicy?.[urgency]?.orderType || 'IOC'}
                          onChange={(event) => updateUrgencyPolicy(urgency, 'orderType', event.target.value)}
                        >
                          {orderTypes.map((orderType) => (
                            <option key={orderType} value={orderType}>{orderType}</option>
                          ))}
                        </select>
                      </div>
                    ))}
                  </div>
                </section>

                <label className="policy-reason-field">
                  <span>{t('policyConfig.form.reason')}</span>
                  <textarea
                    value={reason}
                    onChange={(event) => setReason(event.target.value)}
                    rows={3}
                    required
                  />
                </label>

                <div className="policy-form-actions">
                  <button type="button" className="policy-reset-btn" onClick={handleReset}>
                    {t('policyConfig.actions.reset')}
                  </button>
                  <button type="submit" className="policy-save-btn" disabled={isSaving}>
                    {isSaving ? t('policyConfig.actions.saving') : t('policyConfig.actions.save')}
                  </button>
                </div>
              </>
            )}
          </form>

          <aside className="policy-side-panel">
            <div className="policy-panel-title compact">
              <div>
                <h2>{t('policyConfig.side.title')}</h2>
                <p>{t('policyConfig.side.subtitle')}</p>
              </div>
            </div>

            <section className="policy-impact-card">
              <h3>{t('policyConfig.side.impact')}</h3>
              {(context?.impact || []).slice(0, 6).map((item) => (
                <div className="policy-impact-row" key={item.hedgeFlowId}>
                  <span>
                    <strong>{item.symbol}</strong>
                    <small>{item.hedgeFlowId}</small>
                  </span>
                  <span>
                    <strong>{formatQty(item.gapQty, item.symbol)}</strong>
                    <small>{t('policyConfig.side.threshold', { value: formatNumber(item.thresholdQty, 4) })}</small>
                  </span>
                  <b className={`policy-status policy-status-${impactTone(item)}`}>
                    {item.eligible ? item.urgency : t('policyConfig.side.skip')}
                  </b>
                </div>
              ))}
            </section>

            <section className="policy-audit-card">
              <h3>{t('policyConfig.side.audit')}</h3>
              {(context?.audit || []).map((entry) => (
                <div className="policy-audit-row" key={`${entry.revision}-${entry.updatedAt}`}>
                  <span>
                    <strong>{t('policyConfig.side.revision', { revision: entry.revision })}</strong>
                    <small>{formatDateTime(entry.updatedAt)}</small>
                  </span>
                  <p>{entry.reason}</p>
                  <small>{entry.changedFields.join(', ')}</small>
                </div>
              ))}
            </section>

            <section className="policy-risk-card">
              <h3>{t('policyConfig.side.risk')}</h3>
              <div>
                <span>{t('policyConfig.side.maxThreshold')}</span>
                <strong>{formatNumber(summary.maxThresholdQty, 4)}</strong>
              </div>
              <div>
                <span>{t('policyConfig.side.avgSlippage')}</span>
                <strong>{formatBps(summary.avgMaxSlippageBps)}</strong>
              </div>
              <div>
                <span>{t('policyConfig.side.coverage')}</span>
                <strong>{formatPct(((summary.eligibleFlows || 0) / Math.max((context?.impact || []).length, 1)) * 100)}</strong>
              </div>
            </section>
          </aside>
        </section>
      </main>
    </div>
  );
};

export default PolicyConfig;
