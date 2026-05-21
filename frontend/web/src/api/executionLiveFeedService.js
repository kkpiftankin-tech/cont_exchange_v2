import axios from 'axios';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const API_TIMEOUT = Number(process.env.REACT_APP_API_TIMEOUT || 10000);
const DEFAULT_RECONNECT_MS = 2200;

const api = axios.create({
  baseURL: API_BASE,
  timeout: API_TIMEOUT,
});

const emptyExecutionLiveFeed = {
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

function compactParams(params = {}) {
  return Object.entries(params).reduce((acc, [key, value]) => {
    if (value === undefined || value === null || value === '') return acc;
    if (value === 'all' && key !== 'search') return acc;
    acc[key] = value;
    return acc;
  }, {});
}

function toNumberOrNull(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function asArray(value) {
  return Array.isArray(value) ? value : [];
}

function normalizeExecutionReport(raw = {}) {
  const filledQty = toNumberOrNull(raw.filledQty ?? raw.filled_qty ?? raw.fillQty ?? raw.fill_qty);
  const avgFillPrice = toNumberOrNull(
    raw.avgFillPrice
      ?? raw.avg_fill_price
      ?? raw.avgPrice
      ?? raw.avg_price
      ?? raw.fillPrice
      ?? raw.fill_price
      ?? raw.price
  );
  const notional = toNumberOrNull(raw.notional);

  return {
    ...raw,
    executionId: String(raw.executionId ?? raw.execution_id ?? raw.execId ?? raw.exec_id ?? ''),
    fillId: String(raw.fillId ?? raw.fill_id ?? ''),
    hedgeFlowId: String(raw.hedgeFlowId ?? raw.hedge_flow_id ?? ''),
    childOrderId: String(raw.childOrderId ?? raw.child_order_id ?? ''),
    clientOrderId: String(raw.clientOrderId ?? raw.client_order_id ?? ''),
    batchId: String(raw.batchId ?? raw.batch_id ?? ''),
    providerId: String(raw.providerId ?? raw.provider_id ?? ''),
    symbol: String(raw.symbol ?? ''),
    venueSymbol: String(raw.venueSymbol ?? raw.venue_symbol ?? raw.symbol ?? ''),
    venueId: String(raw.venueId ?? raw.venue_id ?? ''),
    side: String(raw.side ?? '').toUpperCase(),
    status: String(raw.status ?? '').toUpperCase(),
    urgency: String(raw.urgency ?? ''),
    routeType: String(raw.routeType ?? raw.route_type ?? raw.orderType ?? raw.order_type ?? ''),
    sourceTopic: String(raw.sourceTopic ?? raw.source_topic ?? 'execution.venue'),
    timestamp: raw.timestamp ?? raw.ts ?? raw.createdAt ?? raw.created_at ?? '',
    receivedAt: raw.receivedAt ?? raw.received_at ?? '',
    filledQty,
    avgFillPrice,
    referenceMid: toNumberOrNull(raw.referenceMid ?? raw.reference_mid),
    notional: notional ?? (
      Number.isFinite(avgFillPrice) && Number.isFinite(filledQty)
        ? avgFillPrice * filledQty
        : null
    ),
    fee: toNumberOrNull(raw.fee),
    feeCurrency: String(raw.feeCurrency ?? raw.fee_currency ?? 'USDT'),
    slippageBps: toNumberOrNull(raw.slippageBps ?? raw.slippage_bps),
    hedgePnl: toNumberOrNull(raw.hedgePnl ?? raw.hedge_pnl),
    latencyMs: toNumberOrNull(raw.latencyMs ?? raw.latency_ms),
    sequence: raw.sequence,
    live: Boolean(raw.live),
  };
}

export function normalizeExecutionLiveFeedResponse(payload = {}) {
  const filters = payload.filters && typeof payload.filters === 'object'
    ? payload.filters
    : emptyExecutionLiveFeed.filters;
  const items = asArray(payload.items ?? payload.reports ?? payload.executionReports)
    .map(normalizeExecutionReport);

  return {
    ...emptyExecutionLiveFeed,
    ...payload,
    items,
    total: Number.isFinite(Number(payload.total)) ? Number(payload.total) : items.length,
    summary: payload.summary && typeof payload.summary === 'object' ? payload.summary : {},
    filters: {
      symbols: asArray(filters.symbols),
      venues: asArray(filters.venues),
      providers: asArray(filters.providers),
      statuses: asArray(filters.statuses),
      sides: asArray(filters.sides),
    },
    generatedAt: payload.generatedAt ?? payload.generated_at ?? null,
    source: payload.source || '',
  };
}

function buildExecutionLiveFeedUrl(pathname, params = {}) {
  const origin = typeof window !== 'undefined' ? window.location.origin : 'http://localhost';
  const baseUrl = /^https?:\/\//i.test(API_BASE) ? API_BASE : `${origin}${API_BASE}`;
  const url = new URL(pathname.replace(/^\//, ''), `${baseUrl.replace(/\/$/, '')}/`);

  Object.entries(compactParams(params)).forEach(([key, value]) => {
    url.searchParams.set(key, value);
  });

  return url;
}

export function parseExecutionLiveFeedMessage(data) {
  const message = typeof data === 'string' ? JSON.parse(data) : data;
  if (!message || typeof message !== 'object') return null;

  if (message.type === 'snapshot') {
    return {
      ...message,
      payload: normalizeExecutionLiveFeedResponse(message.payload),
    };
  }

  if (message.type === 'execution_report') {
    return {
      ...message,
      item: normalizeExecutionReport(message.item || {}),
      generatedAt: message.generatedAt ?? message.generated_at ?? message.item?.timestamp ?? null,
      source: message.source || '',
    };
  }

  if (message.type === 'heartbeat') {
    return {
      ...message,
      generatedAt: message.generatedAt ?? message.generated_at ?? new Date().toISOString(),
      source: message.source || '',
    };
  }

  return message;
}

export const getExecutionLiveSnapshot = async (params = {}) => {
  const response = await api.get('/executions/live', { params: compactParams(params) });
  return normalizeExecutionLiveFeedResponse(response.data);
};

export function buildExecutionLiveFeedWsUrl(params = {}) {
  const url = buildExecutionLiveFeedUrl('/executions/live/ws', params);
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
  return url.toString();
}

export function subscribeExecutionLiveFeed({
  params = {},
  onSnapshot,
  onReport,
  onHeartbeat,
  onOpen,
  onState,
  onError,
  reconnectMs = DEFAULT_RECONNECT_MS,
} = {}) {
  if (typeof window === 'undefined' || typeof window.WebSocket === 'undefined') {
    onError?.(new Error('Execution Live Feed WebSocket is unavailable.'));
    return () => {};
  }

  let socket = null;
  let reconnectTimer = null;
  let closed = false;

  const clearReconnect = () => {
    if (reconnectTimer) {
      window.clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
  };

  const connect = () => {
    if (closed) return;
    onState?.('connecting');
    socket = new window.WebSocket(buildExecutionLiveFeedWsUrl(params));

    socket.onopen = () => {
      if (closed) return;
      onState?.('connected');
      onOpen?.();
    };

    socket.onmessage = (event) => {
      if (closed) return;
      try {
        const message = parseExecutionLiveFeedMessage(event.data);
        if (!message) return;
        if (message.type === 'snapshot') {
          onSnapshot?.(message.payload, message);
        } else if (message.type === 'execution_report') {
          onReport?.(message.item, message);
        } else if (message.type === 'heartbeat') {
          onHeartbeat?.(message);
        }
      } catch (error) {
        onError?.(error);
      }
    };

    socket.onerror = () => {
      if (!closed) {
        onState?.('error');
        onError?.(new Error('Execution Live Feed WebSocket error.'));
      }
    };

    socket.onclose = () => {
      if (closed) return;
      onState?.('reconnecting');
      clearReconnect();
      reconnectTimer = window.setTimeout(connect, reconnectMs);
    };
  };

  connect();

  return () => {
    closed = true;
    clearReconnect();
    if (socket) socket.close();
  };
}
