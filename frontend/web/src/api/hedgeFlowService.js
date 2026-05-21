import axios from 'axios';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const API_TIMEOUT = Number(process.env.REACT_APP_API_TIMEOUT || 10000);

const api = axios.create({
  baseURL: API_BASE,
  timeout: API_TIMEOUT,
});

const emptyHedgePnlDashboard = {
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

const emptyReconciliationAlerts = {
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

const emptyManualOverrideContext = {
  defaults: {},
  options: {
    symbols: [],
    venues: [],
    providers: [],
    sides: [],
    urgencies: [],
  },
  sourceAlerts: [],
  recent: [],
  summary: {},
  generatedAt: null,
  source: '',
};

const emptyPolicyConfigContext = {
  config: {
    solverConfigId: '',
    revision: 0,
    hedgeTriggerThreshold: {},
    hedgeUrgencyPolicy: {},
    maxSlippageBps: {},
    riskLimits: {},
    updatedBy: '',
    updatedAt: null,
  },
  options: {
    urgencies: [],
    orderTypes: [],
    symbols: [],
  },
  impact: [],
  audit: [],
  summary: {},
  generatedAt: null,
  source: '',
};

function compactParams(params = {}) {
  return Object.entries(params).reduce((acc, [key, value]) => {
    if (value === undefined || value === null || value === '') return acc;
    if (value === 'all') return acc;
    acc[key] = value;
    return acc;
  }, {});
}

function asArray(value) {
  return Array.isArray(value) ? value : [];
}

function numberOrNull(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function numberOrZero(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : 0;
}

function roundNumber(value, digits = 2) {
  const number = Number(value);
  if (!Number.isFinite(number)) return null;
  return Number(number.toFixed(digits));
}

function normalizeHedgePnlSummary(raw = {}) {
  const totalFees = numberOrZero(raw.totalFees ?? raw.total_fees);
  const netAfterFees = numberOrZero(raw.netAfterFees ?? raw.net_after_fees ?? raw.totalPnl ?? raw.total_pnl);
  const grossBeforeFees = numberOrNull(raw.grossBeforeFees ?? raw.gross_before_fees);

  return {
    ...raw,
    totalPnl: numberOrZero(raw.totalPnl ?? raw.total_pnl ?? netAfterFees),
    totalFees,
    netAfterFees,
    grossBeforeFees: grossBeforeFees ?? netAfterFees + totalFees,
    totalNotional: numberOrZero(raw.totalNotional ?? raw.total_notional),
    totalFilledQty: numberOrZero(raw.totalFilledQty ?? raw.total_filled_qty),
    reportCount: numberOrZero(raw.reportCount ?? raw.report_count),
    flowCount: numberOrZero(raw.flowCount ?? raw.flow_count),
    avgSlippageBps: numberOrNull(raw.avgSlippageBps ?? raw.avg_slippage_bps),
    avgPriceSpread: numberOrNull(raw.avgPriceSpread ?? raw.avg_price_spread),
    winRatePct: numberOrZero(raw.winRatePct ?? raw.win_rate_pct),
  };
}

function normalizeHedgePnlRow(raw = {}, cumulativePnl) {
  const avgFillPrice = numberOrNull(raw.avgFillPrice ?? raw.avg_fill_price ?? raw.avgPrice ?? raw.avg_price);
  const clearingPrice = numberOrNull(
    raw.clearingPrice
      ?? raw.clearing_price
      ?? raw.referenceMid
      ?? raw.reference_mid
  );
  const filledQty = numberOrZero(raw.filledQty ?? raw.filled_qty);
  const notional = numberOrNull(raw.notional);
  const hedgePnl = numberOrNull(raw.hedgePnl ?? raw.hedge_pnl);

  return {
    ...raw,
    executionId: String(raw.executionId ?? raw.execution_id ?? ''),
    fillId: String(raw.fillId ?? raw.fill_id ?? ''),
    hedgeFlowId: String(raw.hedgeFlowId ?? raw.hedge_flow_id ?? ''),
    batchId: String(raw.batchId ?? raw.batch_id ?? ''),
    providerId: String(raw.providerId ?? raw.provider_id ?? ''),
    symbol: String(raw.symbol ?? ''),
    venueId: String(raw.venueId ?? raw.venue_id ?? ''),
    side: String(raw.side ?? '').toUpperCase(),
    status: String(raw.status ?? '').toUpperCase(),
    urgency: String(raw.urgency ?? ''),
    timestamp: raw.timestamp ?? raw.ts ?? raw.createdAt ?? raw.created_at ?? '',
    filledQty,
    notional: notional ?? (Number.isFinite(avgFillPrice) ? avgFillPrice * filledQty : 0),
    fee: numberOrZero(raw.fee),
    feeCurrency: String(raw.feeCurrency ?? raw.fee_currency ?? 'USDT'),
    clearingPrice,
    referenceMid: numberOrNull(raw.referenceMid ?? raw.reference_mid ?? clearingPrice),
    avgFillPrice,
    priceSpread: numberOrNull(raw.priceSpread ?? raw.price_spread),
    slippageBps: numberOrNull(raw.slippageBps ?? raw.slippage_bps),
    hedgePnl,
    cumulativePnl: numberOrNull(raw.cumulativePnl ?? raw.cumulative_pnl ?? cumulativePnl),
  };
}

function normalizeHedgePnlBreakdown(raw = {}) {
  return {
    ...raw,
    id: String(raw.id ?? raw.key ?? raw.symbol ?? raw.venueId ?? raw.venue_id ?? 'unknown'),
    hedgePnl: numberOrZero(raw.hedgePnl ?? raw.hedge_pnl),
    fees: numberOrZero(raw.fees ?? raw.totalFees ?? raw.total_fees),
    notional: numberOrZero(raw.notional),
    filledQty: numberOrZero(raw.filledQty ?? raw.filled_qty),
    reports: numberOrZero(raw.reports ?? raw.reportCount ?? raw.report_count),
    avgSlippageBps: numberOrNull(raw.avgSlippageBps ?? raw.avg_slippage_bps),
    avgPriceSpread: numberOrNull(raw.avgPriceSpread ?? raw.avg_price_spread),
    winRatePct: numberOrZero(raw.winRatePct ?? raw.win_rate_pct),
  };
}

function normalizeReconciliationAlert(raw = {}) {
  const venueIds = asArray(raw.venueIds ?? raw.venue_ids ?? raw.venues);

  return {
    ...raw,
    alertId: String(raw.alertId ?? raw.alert_id ?? ''),
    type: String(raw.type ?? raw.alertType ?? raw.alert_type ?? '').toUpperCase(),
    severity: String(raw.severity ?? 'info').toLowerCase(),
    hedgeFlowId: String(raw.hedgeFlowId ?? raw.hedge_flow_id ?? ''),
    intentId: String(raw.intentId ?? raw.intent_id ?? ''),
    batchId: String(raw.batchId ?? raw.batch_id ?? ''),
    providerId: String(raw.providerId ?? raw.provider_id ?? ''),
    symbol: String(raw.symbol ?? ''),
    side: String(raw.side ?? '').toUpperCase(),
    status: String(raw.status ?? '').toUpperCase(),
    reconciliationStatus: String(raw.reconciliationStatus ?? raw.reconciliation_status ?? '').toUpperCase(),
    venueIds: venueIds.map((venue) => String(venue)),
    targetQty: numberOrZero(raw.targetQty ?? raw.target_qty),
    filledQty: numberOrZero(raw.filledQty ?? raw.filled_qty),
    gapQty: numberOrZero(raw.gapQty ?? raw.gap_qty ?? raw.gap),
    gapPct: numberOrZero(raw.gapPct ?? raw.gap_pct),
    targetNotional: numberOrZero(raw.targetNotional ?? raw.target_notional),
    avgFillPrice: numberOrNull(raw.avgFillPrice ?? raw.avg_fill_price),
    referenceMid: numberOrNull(raw.referenceMid ?? raw.reference_mid),
    hedgePnl: numberOrNull(raw.hedgePnl ?? raw.hedge_pnl),
    slippageBps: numberOrNull(raw.slippageBps ?? raw.slippage_bps),
    totalFee: numberOrZero(raw.totalFee ?? raw.total_fee),
    feeCurrency: String(raw.feeCurrency ?? raw.fee_currency ?? 'USDT'),
    urgency: String(raw.urgency ?? ''),
    strategy: String(raw.strategy ?? ''),
    hedgeMode: String(raw.hedgeMode ?? raw.hedge_mode ?? ''),
    timeoutMs: numberOrZero(raw.timeoutMs ?? raw.timeout_ms),
    createdAt: raw.createdAt ?? raw.created_at ?? null,
    updatedAt: raw.updatedAt ?? raw.updated_at ?? null,
    completedAt: raw.completedAt ?? raw.completed_at ?? null,
    timestamp: raw.timestamp ?? raw.ts ?? raw.updatedAt ?? raw.updated_at ?? '',
    nextAction: String(raw.nextAction ?? raw.next_action ?? ''),
    statusReason: String(raw.statusReason ?? raw.status_reason ?? ''),
    riskDecision: String(raw.riskDecision ?? raw.risk_decision ?? ''),
    riskLimitUsagePct: numberOrNull(raw.riskLimitUsagePct ?? raw.risk_limit_usage_pct),
    sourceTopic: String(raw.sourceTopic ?? raw.source_topic ?? 'risk.alerts'),
  };
}

function normalizeReconciliationSummary(raw = {}) {
  return {
    ...raw,
    total: numberOrZero(raw.total),
    critical: numberOrZero(raw.critical),
    warning: numberOrZero(raw.warning),
    underfilled: numberOrZero(raw.underfilled),
    rejected: numberOrZero(raw.rejected),
    totalGapQty: numberOrZero(raw.totalGapQty ?? raw.total_gap_qty),
    avgGapPct: numberOrZero(raw.avgGapPct ?? raw.avg_gap_pct),
    oldestAlertAt: raw.oldestAlertAt ?? raw.oldest_alert_at ?? null,
    latestAlertAt: raw.latestAlertAt ?? raw.latest_alert_at ?? null,
  };
}

function normalizeManualOverrideIntent(raw = {}) {
  return {
    ...raw,
    intentId: String(raw.intentId ?? raw.intent_id ?? ''),
    hedgeFlowId: raw.hedgeFlowId ?? raw.hedge_flow_id ?? null,
    sourceAlertId: String(raw.sourceAlertId ?? raw.source_alert_id ?? ''),
    sourceHedgeFlowId: String(raw.sourceHedgeFlowId ?? raw.source_hedge_flow_id ?? ''),
    batchId: String(raw.batchId ?? raw.batch_id ?? ''),
    providerId: String(raw.providerId ?? raw.provider_id ?? ''),
    symbol: String(raw.symbol ?? ''),
    side: String(raw.side ?? '').toUpperCase(),
    targetQty: numberOrZero(raw.targetQty ?? raw.target_qty),
    targetNotional: numberOrZero(raw.targetNotional ?? raw.target_notional),
    venueIds: asArray(raw.venueIds ?? raw.venue_ids).map((venue) => String(venue)),
    urgency: String(raw.urgency ?? '').toUpperCase(),
    priceConstraint: numberOrNull(raw.priceConstraint ?? raw.price_constraint),
    timeoutMs: numberOrZero(raw.timeoutMs ?? raw.timeout_ms),
    reason: String(raw.reason ?? ''),
    referenceMid: numberOrNull(raw.referenceMid ?? raw.reference_mid),
    maxSlippageBps: numberOrNull(raw.maxSlippageBps ?? raw.max_slippage_bps),
    riskStatus: String(raw.riskStatus ?? raw.risk_status ?? raw.status ?? '').toUpperCase(),
    riskDecision: String(raw.riskDecision ?? raw.risk_decision ?? ''),
    riskReason: String(raw.riskReason ?? raw.risk_reason ?? ''),
    hedgeMode: String(raw.hedgeMode ?? raw.hedge_mode ?? ''),
    routePlan: asArray(raw.routePlan ?? raw.route_plan).map((route = {}) => ({
      ...route,
      venueId: String(route.venueId ?? route.venue_id ?? ''),
      orderType: String(route.orderType ?? route.order_type ?? ''),
      splitQty: numberOrZero(route.splitQty ?? route.split_qty),
      sequence: numberOrZero(route.sequence),
    })),
    sourceTopic: String(raw.sourceTopic ?? raw.source_topic ?? 'execution.intents'),
    createdAt: raw.createdAt ?? raw.created_at ?? null,
    status: String(raw.status ?? raw.riskStatus ?? raw.risk_status ?? '').toUpperCase(),
  };
}

function normalizeManualOverrideSummary(raw = {}) {
  return {
    ...raw,
    sourceAlerts: numberOrZero(raw.sourceAlerts ?? raw.source_alerts),
    recent: numberOrZero(raw.recent),
    accepted: numberOrZero(raw.accepted),
    rejected: numberOrZero(raw.rejected),
  };
}

function normalizePolicyUrgencyPolicy(raw = {}) {
  return Object.entries(raw || {}).reduce((acc, [urgency, value = {}]) => {
    const policy = value && typeof value === 'object' ? value : {};
    acc[String(urgency).toUpperCase()] = {
      ...policy,
      minGapPct: numberOrZero(policy.minGapPct ?? policy.min_gap_pct),
      orderType: String(policy.orderType ?? policy.order_type ?? '').toUpperCase(),
      timeoutMs: numberOrZero(policy.timeoutMs ?? policy.timeout_ms),
    };
    return acc;
  }, {});
}

function normalizeNumberMap(raw = {}) {
  return Object.entries(raw || {}).reduce((acc, [key, value]) => {
    acc[key] = numberOrZero(value);
    return acc;
  }, {});
}

function normalizePolicyConfig(raw = {}) {
  const riskLimits = raw.riskLimits ?? raw.risk_limits ?? {};
  return {
    ...raw,
    solverConfigId: String(raw.solverConfigId ?? raw.solver_config_id ?? ''),
    revision: numberOrZero(raw.revision),
    hedgeTriggerThreshold: normalizeNumberMap(raw.hedgeTriggerThreshold ?? raw.hedge_trigger_threshold),
    hedgeUrgencyPolicy: normalizePolicyUrgencyPolicy(raw.hedgeUrgencyPolicy ?? raw.hedge_urgency_policy),
    maxSlippageBps: normalizeNumberMap(raw.maxSlippageBps ?? raw.max_slippage_bps),
    riskLimits: {
      ...riskLimits,
      hedgeExposureLimit: numberOrZero(
        riskLimits.hedgeExposureLimit
          ?? riskLimits.hedge_exposure_limit
      ),
      maxNotionalPerHedge: numberOrZero(
        riskLimits.maxNotionalPerHedge
          ?? riskLimits.max_notional_per_hedge
      ),
    },
    updatedBy: String(raw.updatedBy ?? raw.updated_by ?? ''),
    updatedAt: raw.updatedAt ?? raw.updated_at ?? null,
  };
}

function normalizePolicyImpact(raw = {}) {
  return {
    ...raw,
    hedgeFlowId: String(raw.hedgeFlowId ?? raw.hedge_flow_id ?? ''),
    symbol: String(raw.symbol ?? ''),
    status: String(raw.status ?? '').toUpperCase(),
    gapQty: numberOrZero(raw.gapQty ?? raw.gap_qty),
    thresholdQty: numberOrZero(raw.thresholdQty ?? raw.threshold_qty),
    gapPct: numberOrZero(raw.gapPct ?? raw.gap_pct),
    eligible: Boolean(raw.eligible),
    urgency: String(raw.urgency ?? '').toUpperCase(),
    maxSlippageBps: numberOrZero(raw.maxSlippageBps ?? raw.max_slippage_bps),
  };
}

function normalizePolicyAudit(raw = {}) {
  return {
    ...raw,
    revision: numberOrZero(raw.revision),
    actor: String(raw.actor ?? ''),
    reason: String(raw.reason ?? ''),
    updatedAt: raw.updatedAt ?? raw.updated_at ?? null,
    changedFields: asArray(raw.changedFields ?? raw.changed_fields).map((field) => String(field)),
  };
}

function normalizePolicySummary(raw = {}) {
  return {
    ...raw,
    revision: numberOrZero(raw.revision),
    symbols: numberOrZero(raw.symbols),
    eligibleFlows: numberOrZero(raw.eligibleFlows ?? raw.eligible_flows),
    avgMaxSlippageBps: numberOrZero(raw.avgMaxSlippageBps ?? raw.avg_max_slippage_bps),
    maxThresholdQty: numberOrZero(raw.maxThresholdQty ?? raw.max_threshold_qty),
    hedgeExposureLimit: numberOrZero(raw.hedgeExposureLimit ?? raw.hedge_exposure_limit),
    maxNotionalPerHedge: numberOrZero(raw.maxNotionalPerHedge ?? raw.max_notional_per_hedge),
  };
}

export function normalizeHedgePnlDashboardResponse(payload = {}) {
  let cumulativePnl = 0;
  const timeSeries = asArray(payload.timeSeries ?? payload.time_series ?? payload.items)
    .map((row) => {
      cumulativePnl += numberOrZero(row.hedgePnl ?? row.hedge_pnl);
      return normalizeHedgePnlRow(row, roundNumber(cumulativePnl));
    });
  const filters = payload.filters && typeof payload.filters === 'object'
    ? payload.filters
    : emptyHedgePnlDashboard.filters;

  return {
    ...emptyHedgePnlDashboard,
    ...payload,
    summary: normalizeHedgePnlSummary(payload.summary),
    timeSeries,
    symbolBreakdown: asArray(payload.symbolBreakdown ?? payload.symbol_breakdown)
      .map(normalizeHedgePnlBreakdown),
    venueBreakdown: asArray(payload.venueBreakdown ?? payload.venue_breakdown)
      .map(normalizeHedgePnlBreakdown),
    filters: {
      symbols: asArray(filters.symbols),
      venues: asArray(filters.venues),
      providers: asArray(filters.providers),
    },
    generatedAt: payload.generatedAt ?? payload.generated_at ?? null,
    source: payload.source || '',
  };
}

export function normalizeReconciliationAlertsResponse(payload = {}) {
  const filters = payload.filters && typeof payload.filters === 'object'
    ? payload.filters
    : emptyReconciliationAlerts.filters;

  return {
    ...emptyReconciliationAlerts,
    ...payload,
    items: asArray(payload.items).map(normalizeReconciliationAlert),
    total: numberOrZero(payload.total),
    summary: normalizeReconciliationSummary(payload.summary),
    filters: {
      symbols: asArray(filters.symbols),
      venues: asArray(filters.venues),
      providers: asArray(filters.providers),
      statuses: asArray(filters.statuses),
      severities: asArray(filters.severities),
      types: asArray(filters.types),
    },
    generatedAt: payload.generatedAt ?? payload.generated_at ?? null,
    source: payload.source || '',
  };
}

export function normalizeManualOverrideContext(payload = {}) {
  const options = payload.options && typeof payload.options === 'object'
    ? payload.options
    : emptyManualOverrideContext.options;

  return {
    ...emptyManualOverrideContext,
    ...payload,
    defaults: payload.defaults && typeof payload.defaults === 'object' ? payload.defaults : {},
    options: {
      symbols: asArray(options.symbols),
      venues: asArray(options.venues),
      providers: asArray(options.providers),
      sides: asArray(options.sides),
      urgencies: asArray(options.urgencies),
    },
    sourceAlerts: asArray(payload.sourceAlerts ?? payload.source_alerts).map(normalizeReconciliationAlert),
    recent: asArray(payload.recent).map(normalizeManualOverrideIntent),
    summary: normalizeManualOverrideSummary(payload.summary),
    generatedAt: payload.generatedAt ?? payload.generated_at ?? null,
    source: payload.source || '',
  };
}

export function normalizePolicyConfigResponse(payload = {}) {
  const options = payload.options && typeof payload.options === 'object'
    ? payload.options
    : emptyPolicyConfigContext.options;

  return {
    ...emptyPolicyConfigContext,
    ...payload,
    config: normalizePolicyConfig(payload.config),
    options: {
      urgencies: asArray(options.urgencies),
      orderTypes: asArray(options.orderTypes ?? options.order_types),
      symbols: asArray(options.symbols),
    },
    impact: asArray(payload.impact).map(normalizePolicyImpact),
    audit: asArray(payload.audit).map(normalizePolicyAudit),
    summary: normalizePolicySummary(payload.summary),
    generatedAt: payload.generatedAt ?? payload.generated_at ?? null,
    source: payload.source || '',
  };
}

export const getHedgeFlows = async (params = {}) => {
  const response = await api.get('/hedgeflows', { params });
  return response.data;
};

export const getHedgeFlowById = async (hedgeFlowId) => {
  const response = await api.get(`/hedgeflows/${encodeURIComponent(hedgeFlowId)}`);
  return response.data;
};

export const getHedgePnlDashboard = async (params = {}) => {
  const response = await api.get('/hedge-pnl', { params: compactParams(params) });
  return normalizeHedgePnlDashboardResponse(response.data);
};

export const getReconciliationAlerts = async (params = {}) => {
  const response = await api.get('/reconciliation-alerts', { params: compactParams(params) });
  return normalizeReconciliationAlertsResponse(response.data);
};

export const getManualOverrideContext = async (params = {}) => {
  const response = await api.get('/manual-overrides', { params: compactParams(params) });
  return normalizeManualOverrideContext(response.data);
};

export const createManualOverride = async (payload = {}) => {
  const response = await api.post('/manual-overrides', payload);
  return {
    ...response.data,
    intent: normalizeManualOverrideIntent(response.data?.intent),
    context: normalizeManualOverrideContext(response.data?.context),
  };
};

export const getPolicyConfig = async () => {
  const response = await api.get('/policy-config');
  return normalizePolicyConfigResponse(response.data);
};

export const updatePolicyConfig = async (payload = {}) => {
  const response = await api.put('/policy-config', payload);
  return normalizePolicyConfigResponse(response.data);
};
