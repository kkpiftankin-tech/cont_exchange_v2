import axios from 'axios';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const API_TIMEOUT = Number(process.env.REACT_APP_API_TIMEOUT || 10000);

const api = axios.create({
  baseURL: API_BASE,
  timeout: API_TIMEOUT,
});

const compareMetricConfig = {
  avgis: { label: 'avgIS', preference: 'lower' },
  totalpnl: { label: 'Total PnL', preference: 'higher' },
  avgpnl: { label: 'Avg PnL', preference: 'higher' },
  stdpnl: { label: 'Std PnL', preference: 'lower' },
  sharpe: { label: 'Sharpe', preference: 'higher' },
  fillrate: { label: 'Fill rate', preference: 'higher' },
  maxdrawdown: { label: 'Max drawdown', preference: 'lower' },
  avgvwap: { label: 'Avg VWAP', preference: 'neutral' },
  avgsolvetime: { label: 'Avg solve time', preference: 'lower' },
};

function normalizeStatus(value) {
  const normalized = String(value || '').trim().toLowerCase();
  return ['pending', 'running', 'completed', 'failed', 'cancelled'].includes(normalized)
    ? normalized
    : 'pending';
}

function computeProgress(progressbatches, totalbatches, progress) {
  const batches = Number(progressbatches);
  const total = Number(totalbatches);
  if (Number.isFinite(batches) && Number.isFinite(total) && total > 0) {
    return Math.max(0, Math.min(100, Number(((batches / total) * 100).toFixed(1))));
  }
  const numericProgress = Number(progress);
  if (Number.isFinite(numericProgress)) return Math.max(0, Math.min(100, numericProgress));
  return 0;
}

export function normalizeReplaySession(raw = {}) {
  const progressbatches = Number(raw.progressbatches ?? raw.processedbatches ?? raw.currentbatch ?? 0);
  const totalbatches = Number(raw.totalbatches ?? raw.total_batches ?? raw.batchtotal ?? 0);
  const status = normalizeStatus(raw.status);

  return {
    ...raw,
    sessionid: String(raw.sessionid || raw.session_id || raw.sessionId || raw.id || ''),
    name: String(raw.name || raw.sessionname || '').trim(),
    instrument: raw.instrument || raw.symbol || raw.market || '',
    instruments: raw.instruments || raw.symbols || [],
    strategy: raw.strategy ?? raw.strategy_json ?? raw.strategyjson ?? null,
    sessionconfigsnapshot: raw.sessionconfigsnapshot ?? raw.session_config_snapshot ?? null,
    solverconfigid: raw.solverconfigid ?? raw.solver_config_id ?? raw.solverConfigId ?? '',
    risklimitsid: raw.risklimitsid ?? raw.risk_limits_id ?? raw.riskLimitsId ?? '',
    daterangefrom: raw.daterangefrom ?? raw.date_range_from ?? raw.dateRangeFrom ?? '',
    daterangeto: raw.daterangeto ?? raw.date_range_to ?? raw.dateRangeTo ?? '',
    status,
    progressbatches: Number.isFinite(progressbatches) ? progressbatches : 0,
    totalbatches: Number.isFinite(totalbatches) ? totalbatches : 0,
    progress: computeProgress(progressbatches, totalbatches, raw.progress),
    errordetails: raw.errordetails || raw.error || raw.message || '',
    errorcode: String(raw.errorcode || raw.error_code || '').toLowerCase(),
    partialsummary: Boolean(raw.partialsummary ?? raw.partial ?? raw.haspartialresults),
    cancancel: status === 'pending' || status === 'running',
    canretry: status === 'failed' || status === 'cancelled',
    createdat: raw.createdat || raw.created_at || raw.createdAt || '',
    updatedat: raw.updatedat || raw.updated_at || raw.updatedAt || raw.lastupdateat || '',
  };
}

function normalizeListResponse(data) {
  if (Array.isArray(data)) return { items: data.map(normalizeReplaySession), total: data.length };
  if (Array.isArray(data?.items)) return { ...data, items: data.items.map(normalizeReplaySession) };
  if (Array.isArray(data?.sessions)) {
    return { items: data.sessions.map(normalizeReplaySession), total: Number(data.total ?? data.sessions.length) };
  }
  return { items: [], total: 0 };
}

function normalizeSummaryPayload(payload = {}) {
  const summary = payload.summary && typeof payload.summary === 'object' ? payload.summary : payload;
  return {
    ...summary,
    sessionid: summary.sessionid || summary.sessionId || payload.sessionid || payload.session_id || '',
    avgis: Number(summary.avgis ?? summary.avgIs ?? summary.avg_is ?? 0),
    totalpnl: Number(summary.totalpnl ?? summary.totalPnL ?? summary.total_pnl ?? 0),
    avgpnl: Number(summary.avgpnl ?? summary.avgPnL ?? summary.avg_pnl ?? 0),
    stdpnl: Number(summary.stdpnl ?? summary.stdPnL ?? summary.std_pnl ?? 0),
    sharpe: Number(summary.sharpe ?? summary.sharpe_ratio ?? 0),
    fillrate: Number(summary.fillrate ?? summary.fillRate ?? summary.avg_fill_rate ?? 0),
    avgvwap: Number(summary.avgvwap ?? summary.avgVwap ?? summary.avg_vwap ?? 0),
    maxdrawdown: Number(summary.maxdrawdown ?? summary.maxDrawdown ?? summary.max_drawdown ?? 0),
    avgsolvetime: Number(summary.avgsolvetime ?? summary.avgSolveTime ?? summary.avg_solve_time_ms ?? 0),
    processedbatches: Number(summary.processedbatches ?? summary.processedBatches ?? summary.processed_batches ?? payload.processed_batches ?? 0),
    totalbatches: Number(summary.totalbatches ?? summary.totalBatches ?? summary.total_batches ?? payload.total_batches ?? 0),
    totalfillevents: Number(summary.totalfillevents ?? summary.totalFillEvents ?? summary.total_fill_events ?? 0),
    failedbatches: Number(summary.failedbatches ?? summary.failedBatches ?? summary.failed_batches ?? payload.failed_batches ?? 0),
    partial: Boolean(summary.partial),
    nodata: Boolean(summary.nodata),
  };
}

function parseJsonPayload(value, fallback) {
  if (value == null || value === '') return fallback;
  if (typeof value !== 'string') return value;
  try {
    return JSON.parse(value);
  } catch {
    return value;
  }
}

function normalizeAgentLog(log = {}) {
  const state = parseJsonPayload(log.state ?? log.state_json ?? log.statejson, null);
  const action = parseJsonPayload(log.action ?? log.action_json ?? log.actionjson, null);
  const fills = parseJsonPayload(log.fills ?? log.fills_json ?? log.fillsjson, []);
  const batchresult = parseJsonPayload(
    log.batchresult ?? log.batch_result ?? log.batch_result_json ?? log.batchresultjson,
    null
  );
  const metrics = parseJsonPayload(log.metrics ?? log.metrics_json ?? log.metricsjson, null);
  const batchseq = Number(log.batchseq ?? log.batchSeq ?? log.batch_seq ?? 0);
  const originalbatchid = String(log.originalbatchid ?? log.originalBatchId ?? log.original_batch_id ?? '');
  const isvalue = Number(log.isvalue ?? log.is_value ?? 0);
  const fillrate = Number(log.fillrate ?? log.fill_rate ?? 0);
  const solvetimeMs = Number(log.solvetime_ms ?? log.solvetimeMs ?? log.solve_time_ms ?? log.solvetimems ?? 0);
  const residualnorm = Number(log.residualnorm ?? log.residual_norm ?? 0);
  const riskstatus = log.riskstatus || log.riskStatus || log.risk_status || '-';
  const solvererrorflag = Boolean(log.solvererrorflag ?? log.solver_error_flag ?? log.solver_error);
  const errorcode = String(log.errorcode ?? log.error_code ?? '');
  return {
    ...log,
    logid: String(log.logid ?? log.logId ?? log.id ?? ''),
    sessionid: String(log.sessionid ?? log.session_id ?? ''),
    batchseq,
    batch_seq: batchseq,
    originalbatchid,
    original_batch_id: originalbatchid,
    pnl: Number(log.pnl ?? 0),
    isvalue,
    is_value: isvalue,
    fillrate,
    fill_rate: fillrate,
    avgvwap: Number(log.avgvwap ?? 0),
    solvetime_ms: solvetimeMs,
    solve_time_ms: solvetimeMs,
    solvetimems: solvetimeMs,
    residualnorm,
    residual_norm: residualnorm,
    reward: Number(log.reward ?? 0),
    riskstatus,
    risk_status: riskstatus,
    solvererrorflag,
    solver_error_flag: solvererrorflag,
    errorcode,
    error_code: errorcode,
    state,
    action,
    fills,
    batchresult,
    metrics,
    sessionconfigsnapshotversion: log.sessionconfigsnapshotversion ?? log.session_config_snapshot_version ?? '',
    timestamp: log.timestamp || log.createdat || log.event_time_ms || '',
  };
}

function readApiError(error) {
  const details = error?.response?.data;
  return {
    status: Number(error?.response?.status || 0),
    code: String(details?.code || details?.errorcode || details?.error_code || '').toLowerCase(),
    message: details?.errordetails || details?.error_details || details?.message || details?.error || error?.message || 'Unknown replay API error',
    details,
  };
}

export function getReplayErrorMeta(error) {
  return readApiError(error);
}

function toFiniteNumber(value) {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : null;
}

function resolveWinner(metricName, valueA, valueB) {
  if (valueA == null || valueB == null) return 'equal';
  if (valueA === valueB) return 'equal';

  const preference = compareMetricConfig[metricName]?.preference || 'higher';
  if (preference === 'neutral') return 'equal';
  if (preference === 'lower') return valueA < valueB ? 'A' : 'B';
  return valueA > valueB ? 'A' : 'B';
}

function normalizeCompareResponse(sessionA, sessionB, payload) {
  if (!payload || typeof payload !== 'object') return { sessionA, sessionB, compatible: false, metrics: [] };

  const diffEntries = payload.diff && typeof payload.diff === 'object'
    ? Object.entries(payload.diff).map(([key, value]) => ({ key, ...value }))
    : [];
  const rawMetrics = Array.isArray(payload.metrics)
    ? payload.metrics
    : Array.isArray(payload.items)
      ? payload.items
      : diffEntries;

  const metrics = rawMetrics.map((metric) => {
    const key = String(metric.key || metric.metric || '').trim().toLowerCase();
    const valueA = toFiniteNumber(metric.valueA ?? metric.a ?? metric.sessionA ?? metric.left ?? metric.A);
    const valueB = toFiniteNumber(metric.valueB ?? metric.b ?? metric.sessionB ?? metric.right ?? metric.B);
    const delta = toFiniteNumber(metric.delta ?? (valueA == null || valueB == null ? null : valueB - valueA));
    const preference = metric.preference || compareMetricConfig[key]?.preference || 'higher';
    return {
      key: key || String(metric.label || 'metric').toLowerCase(),
      label: metric.label || compareMetricConfig[key]?.label || key || 'Metric',
      preference,
      valueA,
      valueB,
      delta,
      better: metric.better || resolveWinner(key, valueA, valueB),
    };
  });

  return {
    sessionA: payload.sessionA || sessionA,
    sessionB: payload.sessionB || sessionB,
    compatible: payload.compatible ?? true,
    reason: payload.reason || '',
    metrics,
  };
}

function parseStreamEvent(rawPayload) {
  if (!rawPayload || typeof rawPayload !== 'object') return null;
  const session = rawPayload.session ? normalizeReplaySession(rawPayload.session) : null;
  const kind = rawPayload.kind || rawPayload.type || rawPayload.event || '';
  const progress = rawPayload.progress || {};
  const sessionid = rawPayload.sessionid || rawPayload.session_id || session?.sessionid || '';
  const batchseq = Number(rawPayload.batchseq ?? rawPayload.batch_seq ?? progress.batch_seq ?? 0);
  const status = rawPayload.status || session?.status || (kind === 'progress' ? 'running' : '');
  const summary = rawPayload.summary
    ? normalizeSummaryPayload({ ...rawPayload.summary, total_batches: rawPayload.total_batches ?? rawPayload.totalbatches })
    : null;
  const totalbatches = Number(rawPayload.totalbatches ?? rawPayload.total_batches ?? progress.total_batches ?? summary?.totalbatches ?? session?.totalbatches ?? 0);
  const summaryProgress = summary ? summary.processedbatches + summary.failedbatches : null;
  const progressbatches = Number(
    rawPayload.progressbatches ??
      rawPayload.progress_batches ??
      (batchseq > 0 ? batchseq : null) ??
      summaryProgress ??
      session?.progressbatches ??
      0
  );
  return {
    ...rawPayload,
    type: rawPayload.type || rawPayload.event || (kind === 'lifecycle' ? `replay.${status}` : 'replay.progress'),
    sessionid,
    status: normalizeStatus(status),
    batchseq,
    totalbatches,
    progressbatches,
    summary,
    session,
    errorcode: String(rawPayload.errorcode || rawPayload.error_code || session?.errorcode || '').toLowerCase(),
    errordetails: rawPayload.errordetails || rawPayload.error_details || session?.errordetails || '',
    transport: rawPayload.transport || '',
  };
}

function buildHttpUrl(pathname, params = {}) {
  const origin = typeof window !== 'undefined' ? window.location.origin : 'http://localhost';
  const baseUrl = API_BASE.startsWith('http') ? API_BASE : `${origin}${API_BASE}`;
  const url = new URL(pathname.replace(/^\//, ''), `${baseUrl.replace(/\/$/, '')}/`);

  Object.entries(params).forEach(([key, value]) => {
    if (value !== undefined && value !== null && value !== '') {
      url.searchParams.set(key, value);
    }
  });

  return url;
}

function buildWebSocketUrl(params = {}) {
  const url = buildHttpUrl('/v1/replay/results/ws', params);
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
  return url.toString();
}

export async function listReplaySessions(params = {}) {
  const response = await api.get('/v1/replay/sessions', { params });
  return normalizeListResponse(response.data);
}

export async function getReplaySession(sessionid) {
  const response = await api.get(`/v1/replay/sessions/${encodeURIComponent(sessionid)}`);
  return normalizeReplaySession(response.data?.session || response.data);
}

export async function createReplaySession(payload) {
  const response = await api.post('/v1/replay/sessions', payload);
  const session = normalizeReplaySession(response.data?.session || response.data);
  return { ...response.data, session };
}

export async function cancelReplaySession(sessionid) {
  const response = await api.delete(`/v1/replay/sessions/${encodeURIComponent(sessionid)}`);
  return normalizeReplaySession(response.data?.session || response.data);
}

export async function retryReplaySession(sessionid, payload = {}) {
  const response = await api.post(`/v1/replay/sessions/${encodeURIComponent(sessionid)}/retry`, payload);
  const session = normalizeReplaySession(response.data?.session || response.data);
  return { ...response.data, session };
}

export async function getReplaySummary(sessionid) {
  const response = await api.get(`/v1/replay/sessions/${encodeURIComponent(sessionid)}/summary`);
  return normalizeSummaryPayload(response.data);
}

export async function getReplayAgentLogs(sessionid, params = {}) {
  const response = await api.get(`/v1/replay/sessions/${encodeURIComponent(sessionid)}/agentlogs`, { params });
  if (Array.isArray(response.data)) {
    return { items: response.data.map(normalizeAgentLog), total: response.data.length };
  }
  const items = Array.isArray(response.data?.items) ? response.data.items.map(normalizeAgentLog) : [];
  return { ...response.data, items, total: Number(response.data?.total ?? items.length) };
}

export async function getReplayAuditDiff(params = {}) {
  const requestParams = {
    sessionid: params.sessionid,
    batchid: params.batchid,
  };

  const response = await api.get('/v1/replay/audit', { params: requestParams });
  if (Array.isArray(response.data)) return { items: response.data, total: response.data.length };
  if (Array.isArray(response.data?.items)) return response.data;
  if (response.data && typeof response.data === 'object') return { items: [response.data], total: 1 };
  return { items: [], total: 0 };
}

export async function getReplayCompare(params = {}) {
  const sessionA = params.sessionA;
  const sessionB = params.sessionB;

  const response = await api.get('/v1/replay/compare', { params: { sessionA, sessionB } });
  return normalizeCompareResponse(sessionA, sessionB, response.data);
}

export function subscribeReplayResults({ sessionid = '', onEvent, onError, onOpen } = {}) {
  if (typeof window === 'undefined') return () => {};

  let closed = false;
  let cleanup = () => {};
  let opened = false;

  const openEventSource = () => {
    if (closed || typeof window.EventSource === 'undefined') {
      onError?.(new Error('Replay result stream is unavailable.'));
      return;
    }

    const source = new window.EventSource(buildHttpUrl('/v1/replay/results/stream', { session_id: sessionid }).toString());
    source.onopen = () => {
      opened = true;
      onOpen?.({ transport: 'sse' });
    };
    source.onmessage = (event) => {
      try {
        const payload = parseStreamEvent(JSON.parse(event.data));
        if (payload) onEvent?.(payload);
      } catch (error) {
        onError?.(error);
      }
    };
    source.onerror = () => {
      source.close();
      onError?.(new Error('Replay SSE stream disconnected.'));
    };
    cleanup = () => source.close();
  };

  try {
    const socket = new window.WebSocket(buildWebSocketUrl({ session_id: sessionid }));
    const handshakeTimer = window.setTimeout(() => {
      if (!opened) {
        socket.close();
      }
    }, 1500);

    socket.onopen = () => {
      opened = true;
      window.clearTimeout(handshakeTimer);
      onOpen?.({ transport: 'websocket' });
    };
    socket.onmessage = (event) => {
      try {
        const payload = parseStreamEvent(JSON.parse(event.data));
        if (payload) onEvent?.(payload);
      } catch (error) {
        onError?.(error);
      }
    };
    socket.onerror = () => {
      window.clearTimeout(handshakeTimer);
      if (!opened) openEventSource();
    };
    socket.onclose = () => {
      window.clearTimeout(handshakeTimer);
      if (!closed && !opened) openEventSource();
    };
    cleanup = () => {
      window.clearTimeout(handshakeTimer);
      socket.close();
    };
  } catch (error) {
    openEventSource();
  }

  return () => {
    closed = true;
    cleanup();
  };
}
