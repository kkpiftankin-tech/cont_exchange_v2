export const STATUS_ORDER = ['all', 'connected', 'stale', 'empty', 'disconnected'];

export const formatPercent = (value, digits = 1) => {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return '—';
  return `${(numeric * 100).toFixed(digits)}%`;
};

export const formatNumber = (value, digits = 2) => {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return '—';
  return numeric.toLocaleString(undefined, {
    minimumFractionDigits: digits,
    maximumFractionDigits: digits,
  });
};

export const formatCompactCurrency = (value) => {
  if (value === null || value === undefined || value === '') return '—';
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return '—';
  return new Intl.NumberFormat(undefined, {
    notation: 'compact',
    maximumFractionDigits: 1,
  }).format(numeric);
};

export const formatDateTime = (value) => {
  if (!value) return '—';
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) return '—';
  return parsed.toLocaleString();
};

export const formatDurationMs = (value) => {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return '—';
  if (numeric >= 1000) {
    return `${(numeric / 1000).toFixed(1)} s`;
  }
  return `${numeric.toFixed(0)} ms`;
};

export const statusTone = (status) => {
  switch (status) {
    case 'connected':
      return 'good';
    case 'stale':
    case 'empty':
      return 'warn';
    case 'disconnected':
      return 'bad';
    default:
      return 'muted';
  }
};

export const recommendationTone = (recommendation) => {
  switch (recommendation) {
    case 'route':
      return 'good';
    case 'watch':
      return 'warn';
    case 'deprioritize':
      return 'muted';
    case 'disable':
      return 'bad';
    default:
      return 'muted';
  }
};

export const computeSummaryCards = (response) => {
  const summary = response?.summary || {};
  return [
    { id: 'total', value: summary.total || 0 },
    { id: 'healthy', value: summary.healthy || 0 },
    { id: 'stale', value: summary.stale || 0 },
    { id: 'disabled', value: summary.disabled || 0 },
  ];
};

export const filterVenues = (items, activeFilter, search) => {
  const normalizedSearch = String(search || '').trim().toLowerCase();

  return (items || []).filter((item) => {
    if (activeFilter && activeFilter !== 'all' && item.status !== activeFilter) {
      return false;
    }

    if (!normalizedSearch) return true;

    const haystack = [
      item.displayName,
      item.venueId,
      item.symbol,
      item.venueType,
      item.region,
    ]
      .filter(Boolean)
      .join(' ')
      .toLowerCase();

    return haystack.includes(normalizedSearch);
  });
};

export const createDefaultVenueConfigDraft = () => ({
  venueId: '',
  adapterMode: 'simulated',
  wsUrl: '',
  restBaseUrl: '',
  rpcUrl: '',
  chainId: '',
  poolAddress: '',
  venueSymbol: 'BTCUSDT',
  curveLevel: 'L3',
  routingMode: 'auto',
  syntheticEnabled: true,
  depthLevels: 20,
  staleThresholdMs: 15000,
  circuitBreakerEnabled: true,
  circuitBreakerErrors: 3,
  circuitBreakerWindowMs: 60000,
  circuitBreakerCooldownMs: 300000,
  isActive: true,
});

export const hydrateVenueConfigDraft = (config = {}, venueId = '') => ({
  ...createDefaultVenueConfigDraft(),
  venueId,
  adapterMode: config.adapterMode || 'simulated',
  wsUrl: config.wsUrl || '',
  restBaseUrl: config.restBaseUrl || '',
  rpcUrl: config.rpcUrl || '',
  chainId: config.chainId || '',
  poolAddress: config.poolAddress || '',
  venueSymbol: config.venueSymbol || 'BTCUSDT',
  curveLevel: config.curveLevel || 'L3',
  routingMode: config.routingMode || 'auto',
  syntheticEnabled: Boolean(config.syntheticEnabled),
  depthLevels: Number(config.depthLevels || 20),
  staleThresholdMs: Number(config.staleThresholdMs || 15000),
  circuitBreakerEnabled: Boolean(config.circuitBreakerEnabled),
  circuitBreakerErrors: Number(config.circuitBreakerErrors || 3),
  circuitBreakerWindowMs: Number(config.circuitBreakerWindowMs || 60000),
  circuitBreakerCooldownMs: Number(config.circuitBreakerCooldownMs || 300000),
  isActive: config.isActive !== false,
});

export const buildVenueConfigPayload = (draft, { includeVenueId = false } = {}) => {
  const payload = {
    adapter_mode: String(draft.adapterMode || '').trim(),
    ws_url: String(draft.wsUrl || '').trim(),
    rest_base_url: String(draft.restBaseUrl || '').trim(),
    rpc_url: String(draft.rpcUrl || '').trim(),
    chain_id: String(draft.chainId || '').trim(),
    pool_address: String(draft.poolAddress || '').trim(),
    venue_symbol: String(draft.venueSymbol || '').trim(),
    curve_level: String(draft.curveLevel || 'L3').trim() || 'L3',
    routing_mode: String(draft.routingMode || 'auto').trim() === 'watch' ? 'watch' : 'auto',
    synthetic_enabled: Boolean(draft.syntheticEnabled),
    depth_levels: Number(draft.depthLevels || 0),
    stale_threshold_ms: Number(draft.staleThresholdMs || 0),
    circuit_breaker_enabled: Boolean(draft.circuitBreakerEnabled),
    circuit_breaker_errors: Number(draft.circuitBreakerErrors || 0),
    circuit_breaker_window_ms: Number(draft.circuitBreakerWindowMs || 0),
    circuit_breaker_cooldown_ms: Number(draft.circuitBreakerCooldownMs || 0),
    is_active: Boolean(draft.isActive),
  };

  if (includeVenueId) {
    payload.venue_id = String(draft.venueId || '').trim();
  }

  return payload;
};
