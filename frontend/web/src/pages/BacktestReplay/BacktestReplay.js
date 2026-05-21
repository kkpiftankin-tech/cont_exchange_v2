import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from 'recharts';
import { useTranslation } from 'react-i18next';
import { logout } from '../../api/authService';
import {
  cancelReplaySession,
  createReplaySession,
  getReplayAgentLogs,
  getReplayAuditDiff,
  getReplayCompare,
  getReplayErrorMeta,
  getReplaySummary,
  listReplaySessions,
  retryReplaySession,
  subscribeReplayResults,
} from '../../api/replayService';
import logo from '../../assets/logo-purple.svg';
import './BacktestReplay.css';

const DEFAULT_STRATEGY = `[
  {
    "symbol": "BTCUSDT",
    "side": "buy",
    "pL": 58000,
    "pH": 62000,
    "qrate": 0.5,
    "qmax": 100,
    "executionwindow": 3600
  }
]`;

const DEFAULT_SOLVER_OVERRIDE = `{
  "batchintervalms": 1000,
  "maxiterations": 128,
  "tolerance": 0.0001,
  "epsilonliquidity": 0.00001,
  "feemodel": {
    "makerfeerate": 0.0002,
    "takerfeerate": 0.0005
  }
}`;

const DEFAULT_RISK_OVERRIDE = `{
  "maxnotional": 1000000,
  "maxposition": 100,
  "maxleverage": 3,
  "maxorderrate": 20,
  "whitelist": ["BTCUSDT"]
}`;

const DEFAULT_FEE_MODEL = `{
  "makerfeerate": 0.0002,
  "takerfeerate": 0.0005
}`;

const STATUS_FILTERS = ['all', 'pending', 'running', 'completed', 'failed', 'cancelled'];
const AUDIT_MODE_ENABLED = process.env.REACT_APP_REPLAY_AUDIT_ENABLED === 'true';
const RESULT_TABS = AUDIT_MODE_ENABLED
  ? ['summary', 'equity', 'batches', 'compare', 'audit']
  : ['summary', 'equity', 'batches', 'compare'];
const PAGE_SIZE = 5;
const BATCH_PAGE_SIZE = 20;
const EQUITY_LOG_LIMIT = 10000;
const COMPARE_METRIC_VIEW = {
  avgis: { digits: 2, suffix: '' },
  totalpnl: { type: 'pnl' },
  avgpnl: { type: 'pnl' },
  stdpnl: { type: 'pnl' },
  sharpe: { digits: 2, suffix: '' },
  fillrate: { digits: 1, suffix: '%' },
  maxdrawdown: { digits: 2, suffix: ' USDT' },
  avgvwap: { digits: 2, suffix: '' },
  avgsolvetime: { digits: 0, suffix: ' ms' },
};

function formatDateTime(value) {
  if (!value) return '-';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;
  return date.toLocaleString();
}

function formatNumber(value, suffix = '', digits = 2) {
  const number = Number(value);
  if (!Number.isFinite(number)) return `-${suffix}`;
  return `${number.toFixed(digits)}${suffix}`;
}

function formatPnl(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return '-';
  const sign = number > 0 ? '+' : '';
  return `${sign}${number.toFixed(2)} USDT`;
}

function formatSigned(value, digits = 4) {
  const number = Number(value);
  if (!Number.isFinite(number)) return '-';
  const sign = number > 0 ? '+' : '';
  return `${sign}${number.toFixed(digits)}`;
}

function summarizeStrategy(strategyValue) {
  if (!strategyValue) return '-';
  let parsed = strategyValue;
  if (typeof strategyValue === 'string') {
    try {
      parsed = JSON.parse(strategyValue);
    } catch (error) {
      return strategyValue.slice(0, 80);
    }
  }
  const flows = Array.isArray(parsed) ? parsed : [];
  if (flows.length === 0) return '-';
  const symbols = [...new Set(flows.map((flow) => flow.symbol).filter(Boolean))].join(', ');
  const sides = [...new Set(flows.map((flow) => flow.side).filter(Boolean))].join('/');
  const totalQmax = flows.reduce((sum, flow) => sum + Number(flow.qmax || 0), 0);
  return `${symbols || '-'} ${sides || '-'} qmax=${Number.isFinite(totalQmax) ? totalQmax : 0}`;
}

function formatCompareMetricValue(metricKey, value) {
  const config = COMPARE_METRIC_VIEW[metricKey] || { digits: 2, suffix: '' };
  if (config.type === 'pnl') return formatPnl(value);
  return formatNumber(value, config.suffix, config.digits);
}

function formatCompareDelta(metricKey, value) {
  const config = COMPARE_METRIC_VIEW[metricKey] || { digits: 2, suffix: '' };
  if (config.type === 'pnl') {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) return '-';
    const sign = numeric > 0 ? '+' : '';
    return `${sign}${numeric.toFixed(2)} USDT`;
  }
  return `${formatSigned(value, config.digits)}${config.suffix}`;
}

function isValidFlowOrderStrategy(value) {
  if (!Array.isArray(value) || value.length === 0) return false;
  return value.every((flow) => {
    if (!flow || typeof flow !== 'object' || Array.isArray(flow)) return false;
    if (!flow.symbol || !flow.side || flow.pL == null || flow.pH == null || flow.qrate == null || flow.qmax == null) {
      return false;
    }
    if (![flow.pL, flow.pH, flow.qrate, flow.qmax].every(isJsonNumber)) return false;
    if (flow.executionwindow != null && !isJsonNumber(flow.executionwindow)) return false;
    const pL = Number(flow.pL);
    const pH = Number(flow.pH);
    const qrate = Number(flow.qrate);
    const qmax = Number(flow.qmax);
    const executionwindow = flow.executionwindow == null ? null : Number(flow.executionwindow);
    if (!['buy', 'sell'].includes(String(flow.side).toLowerCase())) return false;
    if (pL > pH) return false;
    if (qrate <= 0) return false;
    if (qmax <= 0) return false;
    if (executionwindow != null && executionwindow <= 0) return false;
    return true;
  });
}

function hasFields(object, fields) {
  return fields.every((field) => object?.[field] != null);
}

function isJsonNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function isValidSolverOverride(value) {
  return value &&
    typeof value === 'object' &&
    !Array.isArray(value) &&
    hasFields(value, ['batchintervalms', 'maxiterations', 'tolerance', 'epsilonliquidity']) &&
    isJsonNumber(value.batchintervalms) &&
    isJsonNumber(value.maxiterations) &&
    isJsonNumber(value.tolerance) &&
    isJsonNumber(value.epsilonliquidity) &&
    value.batchintervalms > 0 &&
    value.maxiterations > 0 &&
    value.tolerance > 0 &&
    value.epsilonliquidity >= 0;
}

function isValidRiskOverride(value) {
  return value &&
    typeof value === 'object' &&
    !Array.isArray(value) &&
    hasFields(value, ['maxnotional', 'maxposition', 'maxleverage', 'maxorderrate', 'whitelist']) &&
    isJsonNumber(value.maxnotional) &&
    isJsonNumber(value.maxposition) &&
    isJsonNumber(value.maxleverage) &&
    isJsonNumber(value.maxorderrate) &&
    value.maxnotional > 0 &&
    value.maxposition > 0 &&
    value.maxleverage > 0 &&
    value.maxorderrate > 0 &&
    Array.isArray(value.whitelist) &&
    value.whitelist.every((symbol) => typeof symbol === 'string' && symbol.trim());
}

function isValidFeeModel(value) {
  return value &&
    typeof value === 'object' &&
    !Array.isArray(value) &&
    hasFields(value, ['makerfeerate', 'takerfeerate']) &&
    isJsonNumber(value.makerfeerate) &&
    isJsonNumber(value.takerfeerate) &&
    value.makerfeerate >= 0 &&
    value.takerfeerate >= 0;
}

function optionalJson(raw, enabled) {
  if (!enabled) return undefined;
  const trimmed = String(raw || '').trim();
  return trimmed ? JSON.parse(trimmed) : undefined;
}

function buildPayload(formValues, parsedStrategy) {
  const solverOverride = optionalJson(formValues.solverConfigJson, formValues.solverConfigMode === 'override');
  const riskOverride = optionalJson(formValues.riskLimitsJson, formValues.riskLimitsMode === 'override');
  const feeModel = formValues.feeModelMode === 'production'
    ? { production: true }
    : optionalJson(formValues.feeModelJson, true);
  const instruments = formValues.instruments;

  return {
    name: formValues.name.trim(),
    instruments,
    daterangefrom: formValues.dateFrom,
    daterangeto: formValues.dateTo,
    strategy: parsedStrategy,
    solverconfigid: formValues.solverConfigMode === 'production' ? formValues.solverConfig : undefined,
    solverconfig: solverOverride,
    risklimitsid: formValues.riskLimitsMode === 'production' ? formValues.riskLimits : undefined,
    risklimits: riskOverride,
    feemodel: feeModel,
    rewardmode: formValues.reward,
    sessionconfigsnapshot: {
      solverconfig: solverOverride || formValues.solverConfig,
      risklimits: riskOverride || formValues.riskLimits,
      feemodel: feeModel,
      rewardconfig: { mode: formValues.reward },
      randomseed: Number(formValues.randomSeed),
      tolerance: Number(formValues.tolerance),
    },
  };
}

function sortSessions(items) {
  return [...items].sort((left, right) => {
    const leftTs = Date.parse(left.createdat || 0) || 0;
    const rightTs = Date.parse(right.createdat || 0) || 0;
    return rightTs - leftTs;
  });
}

function mergeSessionUpdate(items, nextSession) {
  if (!nextSession?.sessionid) return items;
  const existing = items.find((item) => item.sessionid === nextSession.sessionid);
  if (!existing) return sortSessions([nextSession, ...items]);
  return sortSessions(items.map((item) => (
    item.sessionid === nextSession.sessionid
      ? { ...item, ...nextSession }
      : item
  )));
}

function getSessionNotice({ selectedSession, summary, agentLogs, t }) {
  if (!selectedSession?.sessionid) return null;

  if (selectedSession.errorcode === 'permission_denied') {
    return { tone: 'bad', message: t('replay.states.permissionDenied') };
  }
  if (selectedSession.errorcode === 'solver_error' || agentLogs.some((item) => item.solvererrorflag)) {
    return {
      tone: 'bad',
      message: selectedSession.errordetails || t('replay.states.solverError'),
    };
  }
  if (summary?.nodata || selectedSession.errorcode === 'no_data') {
    return { tone: 'warn', message: t('replay.states.noData') };
  }
  if (selectedSession.status === 'cancelled') {
    return {
      tone: 'warn',
      message: selectedSession.partialsummary || summary?.partial
        ? t('replay.states.cancelledPartial')
        : t('replay.status.cancelled'),
    };
  }
  if (selectedSession.partialsummary || summary?.partial) {
    return { tone: 'warn', message: t('replay.states.partialSummary') };
  }
  if (selectedSession.status === 'pending') {
    return { tone: 'info', message: t('replay.states.pending') };
  }
  if (selectedSession.status === 'running') {
    return { tone: 'info', message: t('replay.states.liveRunning') };
  }

  return null;
}

function getErrorMessage(error, t, fallbackKey) {
  const meta = getReplayErrorMeta(error);
  if (meta.status === 403 || meta.code === 'permission_denied') return t('replay.states.permissionDenied');
  if (meta.code === 'solver_error') return t('replay.states.solverError');
  if (meta.code === 'no_data') return t('replay.states.noData');
  if (meta.code === 'validation_error') return t('replay.states.validationError');
  return t(fallbackKey);
}

const BacktestReplay = () => {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const [strategy, setStrategy] = useState(DEFAULT_STRATEGY);
  const [formValues, setFormValues] = useState({
    name: 'BTC replay validation',
    instruments: ['BTCUSDT'],
    dateFrom: '2026-04-01',
    dateTo: '2026-04-07',
    solverConfigMode: 'production',
    solverConfig: 'solver-prod-v4',
    solverConfigJson: DEFAULT_SOLVER_OVERRIDE,
    riskLimitsMode: 'production',
    riskLimits: 'risk-standard',
    riskLimitsJson: DEFAULT_RISK_OVERRIDE,
    feeModelMode: 'production',
    feeModelJson: DEFAULT_FEE_MODEL,
    reward: 'incrementalPnL',
    randomSeed: '42',
    tolerance: '0.0001',
  });
  const [errors, setErrors] = useState({});
  const [submitState, setSubmitState] = useState({ loading: false, message: '', error: '' });
  const [sessions, setSessions] = useState([]);
  const [sessionsState, setSessionsState] = useState({ loading: true, error: '' });
  const [selectedSessionId, setSelectedSessionId] = useState('');
  const [statusFilter, setStatusFilter] = useState('all');
  const [search, setSearch] = useState('');
  const [page, setPage] = useState(1);
  const [summary, setSummary] = useState(null);
  const [agentLogs, setAgentLogs] = useState([]);
  const [equityLogs, setEquityLogs] = useState([]);
  const [agentLogsTotal, setAgentLogsTotal] = useState(0);
  const [resultsState, setResultsState] = useState({ loading: false, error: '' });
  const [sessionActionState, setSessionActionState] = useState({ loading: '', error: '', message: '' });
  const [liveState, setLiveState] = useState({ transport: '', connected: false, lastEventAt: '' });
  const [auditBatchId, setAuditBatchId] = useState('');
  const [auditDiffs, setAuditDiffs] = useState([]);
  const [auditState, setAuditState] = useState({ loading: false, error: '', message: '' });
  const [selectedAuditBatchId, setSelectedAuditBatchId] = useState('');
  const [compareSelection, setCompareSelection] = useState({ sessionA: '', sessionB: '' });
  const [compareResult, setCompareResult] = useState(null);
  const [compareState, setCompareState] = useState({ loading: false, error: '', message: '' });
  const [activeTab, setActiveTab] = useState('summary');
  const [batchFilters, setBatchFilters] = useState({
    batchseq: '',
    pnl: '',
    isvalue: '',
    fillrate: '',
    solver_error: '',
  });
  const [batchPage, setBatchPage] = useState(1);
  const [selectedLog, setSelectedLog] = useState(null);

  const selectedSession = useMemo(
    () => sessions.find((session) => session.sessionid === selectedSessionId) || sessions[0] || null,
    [sessions, selectedSessionId]
  );

  const filteredSessions = useMemo(() => {
    const query = search.trim().toLowerCase();
    return sessions.filter((session) => {
      const statusMatches = statusFilter === 'all' || session.status === statusFilter;
      const queryMatches = !query ||
        String(session.sessionid).toLowerCase().includes(query) ||
        String(session.name || '').toLowerCase().includes(query) ||
        String(session.instrument || '').toLowerCase().includes(query);
      return statusMatches && queryMatches;
    });
  }, [search, sessions, statusFilter]);

  const totalPages = Math.max(1, Math.ceil(filteredSessions.length / PAGE_SIZE));
  const currentPage = Math.min(page, totalPages);
  const pagedSessions = filteredSessions.slice((currentPage - 1) * PAGE_SIZE, currentPage * PAGE_SIZE);
  const batchTotalPages = Math.max(1, Math.ceil(agentLogsTotal / BATCH_PAGE_SIZE));
  const currentBatchPage = Math.min(batchPage, batchTotalPages);

  const equityData = useMemo(() => {
    let cumulative = 0;
    return equityLogs.map((log) => {
      cumulative += Number(log.pnl || 0);
      return {
        batchseq: log.batchseq,
        equity: Number(cumulative.toFixed(2)),
      };
    });
  }, [equityLogs]);

  const selectedAuditDiff = useMemo(() => (
    auditDiffs.find((diff) => diff.batchid === selectedAuditBatchId) || auditDiffs[0] || null
  ), [auditDiffs, selectedAuditBatchId]);

  const sessionNameById = useMemo(
    () => Object.fromEntries(sessions.map((session) => [session.sessionid, session.name || session.sessionid])),
    [sessions]
  );

  const compareRows = useMemo(() => (compareResult?.metrics || []).map((metric) => ({
    ...metric,
    key: String(metric.key || '').toLowerCase(),
  })), [compareResult]);

  const summaryCards = useMemo(() => ([
    { key: 'avgIs', value: formatNumber(summary?.avgis, '', 2) },
    { key: 'totalPnl', value: formatPnl(summary?.totalpnl) },
    { key: 'sharpe', value: formatNumber(summary?.sharpe, '', 2) },
    { key: 'fillRate', value: formatNumber(summary?.fillrate, '%', 1) },
    { key: 'maxDrawdown', value: formatNumber(summary?.maxdrawdown, ' USDT', 2) },
    { key: 'avgSolveTime', value: formatNumber(summary?.avgsolvetime, ' ms', 0) },
    { key: 'totalBatches', value: formatNumber(summary?.totalbatches, '', 0) },
    { key: 'totalFillEvents', value: formatNumber(summary?.totalfillevents, '', 0) },
    { key: 'processedBatches', value: formatNumber(summary?.processedbatches, '', 0) },
    { key: 'failedBatches', value: formatNumber(summary?.failedbatches, '', 0) },
    { key: 'partial', value: summary?.partial ? 'true' : 'false' },
  ]), [summary]);

  const filteredAgentLogs = useMemo(() => agentLogs.filter((log) => {
    if (batchFilters.batchseq && !String(log.batchseq).includes(batchFilters.batchseq)) return false;
    if (batchFilters.pnl && !String(log.pnl).includes(batchFilters.pnl)) return false;
    if (batchFilters.isvalue && !String(log.isvalue).includes(batchFilters.isvalue)) return false;
    if (batchFilters.fillrate && !String(log.fillrate).includes(batchFilters.fillrate)) return false;
    if (batchFilters.solver_error) {
      const expected = batchFilters.solver_error === 'true';
      if (Boolean(log.solvererrorflag) !== expected) return false;
    }
    return true;
  }), [agentLogs, batchFilters]);

  const isFormSubmittable = useMemo(() => {
    if (!formValues.name.trim() || formValues.instruments.length === 0) return false;
    if (!formValues.dateFrom || !formValues.dateTo || formValues.dateFrom > formValues.dateTo) return false;
    if (!Number.isFinite(Number(formValues.randomSeed))) return false;
    if (!Number.isFinite(Number(formValues.tolerance))) return false;
    try {
      const parsed = JSON.parse(strategy);
      if (!isValidFlowOrderStrategy(parsed)) return false;
      if (formValues.solverConfigMode === 'override') {
        if (!isValidSolverOverride(JSON.parse(formValues.solverConfigJson))) return false;
      }
      if (formValues.riskLimitsMode === 'override') {
        if (!isValidRiskOverride(JSON.parse(formValues.riskLimitsJson))) return false;
      }
      if (formValues.feeModelMode === 'override') {
        if (!isValidFeeModel(JSON.parse(formValues.feeModelJson))) return false;
      }
    } catch (error) {
      return false;
    }
    return true;
  }, [formValues, strategy]);

  const sessionNotice = useMemo(
    () => getSessionNotice({ selectedSession, summary, agentLogs, t }),
    [agentLogs, selectedSession, summary, t]
  );

  const noAgentLogsMessage = useMemo(() => {
    if (selectedSession?.status === 'pending' || selectedSession?.status === 'running') {
      return t('replay.states.waitingForLogs');
    }
    if (summary?.nodata) return t('replay.states.noData');
    return t('replay.states.noAgentLogs');
  }, [selectedSession?.status, summary?.nodata, t]);

  const loadSessions = useCallback(async () => {
    setSessionsState((current) => ({ ...current, loading: true, error: '' }));
    try {
      const response = await listReplaySessions({ limit: 50 });
      const items = sortSessions(response.items || []);
      setSessions(items);
      setSelectedSessionId((current) => current || items[0]?.sessionid || '');
      setSessionsState({ loading: false, error: '' });
    } catch (error) {
      setSessionsState({ loading: false, error: t('replay.states.sessionsError') });
    }
  }, [t]);

  const loadResults = useCallback(async (sessionid) => {
    if (!sessionid) {
      setSummary(null);
      setAgentLogs([]);
      setEquityLogs([]);
      setAgentLogsTotal(0);
      setSelectedLog(null);
      return;
    }

    setResultsState({ loading: true, error: '' });
    try {
      const [summaryResponse, logsResponse, equityLogsResponse] = await Promise.all([
        getReplaySummary(sessionid),
        getReplayAgentLogs(sessionid, {
          limit: BATCH_PAGE_SIZE,
          offset: (batchPage - 1) * BATCH_PAGE_SIZE,
          batchseq: batchFilters.batchseq || undefined,
          pnl: batchFilters.pnl || undefined,
          isvalue: batchFilters.isvalue || undefined,
          fillrate: batchFilters.fillrate || undefined,
          solver_error: batchFilters.solver_error || undefined,
        }),
        getReplayAgentLogs(sessionid, {
          limit: EQUITY_LOG_LIMIT,
          offset: 0,
        }),
      ]);
      setSummary(summaryResponse);
      setAgentLogs(logsResponse.items || []);
      setEquityLogs(equityLogsResponse.items || []);
      setAgentLogsTotal(Number(logsResponse.total || 0));
      setSelectedLog(null);
      setResultsState({ loading: false, error: '' });
    } catch (error) {
      setResultsState({ loading: false, error: t('replay.states.resultsError') });
    }
  }, [batchFilters, batchPage, t]);

  useEffect(() => {
    loadSessions();
  }, [loadSessions]);

  useEffect(() => {
    setPage(1);
  }, [search, statusFilter]);

  useEffect(() => {
    setBatchPage(1);
  }, [batchFilters, selectedSession?.sessionid]);

  useEffect(() => {
    if (selectedSession?.sessionid) {
      loadResults(selectedSession.sessionid);
    } else {
      setSummary(null);
      setAgentLogs([]);
      setEquityLogs([]);
      setAgentLogsTotal(0);
    }
  }, [loadResults, selectedSession?.sessionid]);

  useEffect(() => {
    setAuditBatchId('');
    setAuditDiffs([]);
    setSelectedAuditBatchId('');
    setAuditState({ loading: false, error: '', message: '' });
  }, [selectedSession?.sessionid]);

  useEffect(() => {
    setCompareSelection((current) => {
      const available = sessions.map((session) => session.sessionid).filter(Boolean);
      if (available.length === 0) return { sessionA: '', sessionB: '' };
      const sessionA = available.includes(current.sessionA) ? current.sessionA : available[0];
      const sessionB = available.includes(current.sessionB) && current.sessionB !== sessionA
        ? current.sessionB
        : available.find((sessionId) => sessionId !== sessionA) || sessionA;
      return { sessionA, sessionB };
    });
  }, [sessions]);

  useEffect(() => {
    const unsubscribe = subscribeReplayResults({
      sessionid: selectedSession?.sessionid || '',
      onOpen: ({ transport }) => {
        setLiveState({ transport, connected: true, lastEventAt: new Date().toISOString() });
      },
      onError: () => {
        setLiveState((current) => ({ ...current, connected: false }));
      },
      onEvent: (event) => {
        setLiveState((current) => ({
          transport: event.transport || current.transport || 'stream',
          connected: true,
          lastEventAt: new Date().toISOString(),
        }));

        if (event.type === 'replay.sync_required') {
          loadSessions();
          if (selectedSession?.sessionid) {
            loadResults(selectedSession.sessionid);
          }
          return;
        }

        const eventSessionId = event.sessionid || event.session?.sessionid;
        const eventSession = event.session || (eventSessionId ? {
          sessionid: eventSessionId,
          status: event.status,
          progressbatches: event.progressbatches,
          totalbatches: event.totalbatches,
          progress: event.totalbatches > 0 ? (event.progressbatches / event.totalbatches) * 100 : 0,
          errordetails: event.errordetails,
          errorcode: event.errorcode,
          partialsummary: event.summary?.partial,
        } : null);

        if (eventSession) {
          setSessions((current) => mergeSessionUpdate(current, eventSession));
        }

        if (eventSessionId && selectedSession?.sessionid === eventSessionId) {
          if (event.summary) setSummary(event.summary);
          loadResults(eventSessionId);
        }
      },
    });

    return unsubscribe;
  }, [loadResults, loadSessions, selectedSession?.sessionid]);

  const handleLogout = () => {
    logout();
    navigate('/login');
  };

  const handleFieldChange = (event) => {
    const { name, value } = event.target;
    setFormValues((current) => ({ ...current, [name]: value }));
    setErrors((current) => ({ ...current, [name]: '' }));
    if (submitState.error) {
      setSubmitState((current) => ({ ...current, error: '' }));
    }
  };

  const handleInstrumentChange = (event) => {
    const values = Array.from(event.target.selectedOptions).map((option) => option.value);
    setFormValues((current) => ({ ...current, instruments: values }));
    setErrors((current) => ({ ...current, instruments: '' }));
  };

  const handleBatchFilterChange = (event) => {
    const { name, value } = event.target;
    setBatchFilters((current) => ({ ...current, [name]: value }));
  };

  const validateFlowOrderStrategy = (value) => {
    if (!Array.isArray(value) || value.length === 0) {
      return t('replay.form.strategyError');
    }

    for (let index = 0; index < value.length; index += 1) {
      const flow = value[index];
      if (!flow || typeof flow !== 'object' || Array.isArray(flow)) return t('replay.form.strategyError');
      if (!flow.symbol || !flow.side || flow.pL == null || flow.pH == null || flow.qrate == null || flow.qmax == null) {
        return t('replay.form.strategyError');
      }
      if (![flow.pL, flow.pH, flow.qrate, flow.qmax].every(isJsonNumber)) return t('replay.form.strategyError');
      if (flow.executionwindow != null && !isJsonNumber(flow.executionwindow)) return t('replay.form.executionWindowError');
      if (!['buy', 'sell'].includes(String(flow.side).toLowerCase())) return t('replay.form.strategyError');
      if (Number(flow.pL) > Number(flow.pH)) return t('replay.form.priceRangeError');
      if (Number(flow.qrate) <= 0) return t('replay.form.qrateError');
      if (Number(flow.qmax) <= 0) return t('replay.form.qmaxError');
      if (flow.executionwindow != null && Number(flow.executionwindow) <= 0) return t('replay.form.executionWindowError');
    }

    return '';
  };

  const validateOverrideJson = (raw, requiredFields) => {
    try {
      const parsed = JSON.parse(raw);
      if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
        return { error: t('replay.form.jsonObjectError') };
      }
      const missing = requiredFields.find((field) => parsed[field] == null);
      if (missing) return { error: t('replay.form.requiredFieldError') };
      return { parsed };
    } catch (error) {
      return { error: t('replay.form.jsonError') };
    }
  };

  const validateForm = () => {
    const nextErrors = {};
    let parsedStrategy = null;

    if (!formValues.name.trim()) nextErrors.name = t('replay.form.nameError');
    if (!formValues.instruments.length) nextErrors.instruments = t('replay.form.instrumentError');
    if (!formValues.dateFrom) nextErrors.dateFrom = t('replay.form.dateError');
    if (!formValues.dateTo) nextErrors.dateTo = t('replay.form.dateError');
    if (formValues.dateFrom && formValues.dateTo && formValues.dateFrom > formValues.dateTo) {
      nextErrors.dateTo = t('replay.form.dateRangeError');
    }
    if (!Number.isFinite(Number(formValues.randomSeed))) nextErrors.randomSeed = t('replay.form.numberError');
    if (!Number.isFinite(Number(formValues.tolerance))) nextErrors.tolerance = t('replay.form.numberError');

    try {
      parsedStrategy = JSON.parse(strategy);
      const strategyError = validateFlowOrderStrategy(parsedStrategy);
      if (strategyError) nextErrors.strategy = strategyError;
    } catch (error) {
      nextErrors.strategy = t('replay.form.strategyError');
    }
    if (formValues.solverConfigMode === 'override') {
      const { error, parsed } = validateOverrideJson(formValues.solverConfigJson, [
        'batchintervalms',
        'maxiterations',
        'tolerance',
        'epsilonliquidity',
      ]);
      if (error) nextErrors.solverConfigJson = error;
      if (!error && parsed && !isValidSolverOverride(parsed)) {
        nextErrors.solverConfigJson = t('replay.form.numberError');
      }
    }
    if (formValues.riskLimitsMode === 'override') {
      const { error, parsed } = validateOverrideJson(formValues.riskLimitsJson, [
        'maxnotional',
        'maxposition',
        'maxleverage',
        'maxorderrate',
        'whitelist',
      ]);
      if (error) nextErrors.riskLimitsJson = error;
      if (!error && parsed && !isValidRiskOverride(parsed)) {
        nextErrors.riskLimitsJson = t('replay.form.numberError');
      }
    }
    if (formValues.feeModelMode === 'override') {
      const { error, parsed } = validateOverrideJson(formValues.feeModelJson, [
        'makerfeerate',
        'takerfeerate',
      ]);
      if (error) nextErrors.feeModelJson = error;
      if (!error && parsed && !isValidFeeModel(parsed)) {
        nextErrors.feeModelJson = t('replay.form.feeError');
      }
    }

    setErrors(nextErrors);
    return { isValid: Object.keys(nextErrors).length === 0, parsedStrategy };
  };

  const handleCreateSession = async (event) => {
    event.preventDefault();
    setSubmitState({ loading: false, message: '', error: '' });

    const { isValid, parsedStrategy } = validateForm();
    if (!isValid) return;

    setSubmitState({ loading: true, message: '', error: '' });
    try {
      const created = await createReplaySession(buildPayload(formValues, parsedStrategy));
      const createdSession = created.session || created;
      setSessions((current) => mergeSessionUpdate(current, createdSession));
      setSelectedSessionId(createdSession.sessionid);
      setSubmitState({ loading: false, message: t('replay.states.created'), error: '' });
      loadResults(createdSession.sessionid);
    } catch (error) {
      setSubmitState({
        loading: false,
        message: '',
        error: getErrorMessage(error, t, 'replay.states.createError'),
      });
    }
  };

  const handleCancelSession = async () => {
    if (!selectedSession?.sessionid) return;
    setSessionActionState({ loading: 'cancel', error: '', message: '' });
    try {
      const updatedSession = await cancelReplaySession(selectedSession.sessionid);
      setSessions((current) => mergeSessionUpdate(current, updatedSession));
      setSessionActionState({ loading: '', error: '', message: t('replay.states.cancelled') });
      loadResults(updatedSession.sessionid);
    } catch (error) {
      setSessionActionState({
        loading: '',
        error: getErrorMessage(error, t, 'replay.states.cancelError'),
        message: '',
      });
    }
  };

  const handleRetrySession = async () => {
    if (!selectedSession?.sessionid) return;
    setSessionActionState({ loading: 'retry', error: '', message: '' });
    try {
      const response = await retryReplaySession(selectedSession.sessionid);
      const nextSession = response.session || response;
      setSessions((current) => mergeSessionUpdate(current, nextSession));
      setSelectedSessionId(nextSession.sessionid);
      setSessionActionState({ loading: '', error: '', message: t('replay.states.retryStarted') });
      loadResults(nextSession.sessionid);
    } catch (error) {
      setSessionActionState({
        loading: '',
        error: getErrorMessage(error, t, 'replay.states.retryError'),
        message: '',
      });
    }
  };

  const handleRunAudit = async (event) => {
    event.preventDefault();

    if (!selectedSession?.sessionid) {
      setAuditState({ loading: false, error: t('replay.audit.errors.selectSession'), message: '' });
      return;
    }
    if (!auditBatchId.trim()) {
      setAuditState({ loading: false, error: t('replay.audit.errors.batchIdRequired'), message: '' });
      return;
    }

    setAuditState({ loading: true, error: '', message: '' });
    try {
      const response = await getReplayAuditDiff({
        sessionid: selectedSession.sessionid,
        batchid: auditBatchId.trim(),
      });
      const items = response.items || [];
      setAuditDiffs(items);
      setSelectedAuditBatchId(items[0]?.batchid || '');
      if (items.length === 0) {
        setAuditState({ loading: false, error: t('replay.audit.errors.notFound'), message: '' });
        return;
      }
      setAuditState({
        loading: false,
        error: '',
        message: t('replay.audit.loaded', { count: items.length }),
      });
    } catch (error) {
      setAuditState({ loading: false, error: t('replay.audit.errors.loadFailed'), message: '' });
    }
  };

  const handleCompareChange = (event) => {
    const { name, value } = event.target;
    setCompareSelection((current) => ({ ...current, [name]: value }));
    if (compareState.error) {
      setCompareState((current) => ({ ...current, error: '' }));
    }
  };

  const handleRunCompare = async (event) => {
    event.preventDefault();

    if (!compareSelection.sessionA || !compareSelection.sessionB) {
      setCompareState({ loading: false, error: t('replay.compare.errors.selectSessions'), message: '' });
      return;
    }
    if (compareSelection.sessionA === compareSelection.sessionB) {
      setCompareState({ loading: false, error: t('replay.compare.errors.sameSession'), message: '' });
      return;
    }

    setCompareState({ loading: true, error: '', message: '' });
    try {
      const response = await getReplayCompare(compareSelection);
      if (response.compatible === false) {
        setCompareResult(response);
        setCompareState({
          loading: false,
          error: response.reason || t('replay.compare.errors.incompatible'),
          message: '',
        });
        return;
      }
      setCompareResult(response);
      setCompareState({ loading: false, error: '', message: t('replay.compare.loaded') });
    } catch (error) {
      setCompareState({ loading: false, error: t('replay.compare.errors.loadFailed'), message: '' });
    }
  };

  return (
    <div className="replay-page">
      <nav className="navbar-main replay-navbar">
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
          <a href="/manual-override">{t('navbar.manualOverride')}</a>
          <a href="/policy-config">{t('navbar.policyConfig')}</a>
          <a href="/replay" className="active">{t('navbar.replay')}</a>
          <button onClick={handleLogout} className="logout-btn">{t('navbar.logout')}</button>
        </div>
      </nav>

      <main className="replay-shell">
        <section className="replay-workspace" aria-label={t('replay.title')}>
          <form className="replay-launch-panel" onSubmit={handleCreateSession}>
            <div className="replay-panel-header">
              <span>{t('replay.kicker')}</span>
              <h1>{t('replay.title')}</h1>
            </div>

            <div className="replay-form-grid">
              <label>
                {t('replay.form.name')}
                <input name="name" value={formValues.name} onChange={handleFieldChange} />
                {errors.name && <strong>{errors.name}</strong>}
              </label>
              <label>
                {t('replay.form.instrument')}
                <select
                  name="instruments"
                  multiple
                  value={formValues.instruments}
                  onChange={handleInstrumentChange}
                >
                  <option value="BTCUSDT">BTCUSDT</option>
                  <option value="ETHUSDT">ETHUSDT</option>
                  <option value="SOLUSDT">SOLUSDT</option>
                </select>
                {errors.instruments && <strong>{errors.instruments}</strong>}
              </label>
              <label>
                {t('replay.form.dateFrom')}
                <input type="date" name="dateFrom" value={formValues.dateFrom} onChange={handleFieldChange} />
                {errors.dateFrom && <strong>{errors.dateFrom}</strong>}
              </label>
              <label>
                {t('replay.form.dateTo')}
                <input type="date" name="dateTo" value={formValues.dateTo} onChange={handleFieldChange} />
                {errors.dateTo && <strong>{errors.dateTo}</strong>}
              </label>
            </div>

            <label className="replay-strategy-editor">
              <span>{t('replay.form.strategy')}</span>
              <textarea value={strategy} onChange={(event) => setStrategy(event.target.value)} spellCheck="false" />
              {errors.strategy && <strong>{errors.strategy}</strong>}
            </label>

            <div className="replay-form-grid replay-config-grid">
              <label>
                {t('replay.form.solver')}
                <select name="solverConfigMode" value={formValues.solverConfigMode} onChange={handleFieldChange}>
                  <option value="production">{t('replay.form.productionConfig')}</option>
                  <option value="override">{t('replay.form.overrideConfig')}</option>
                </select>
                <select name="solverConfig" value={formValues.solverConfig} onChange={handleFieldChange}>
                  <option>solver-prod-v4</option>
                  <option>solver-low-latency</option>
                  <option>solver-audit-safe</option>
                </select>
                {formValues.solverConfigMode === 'override' && (
                  <>
                    <textarea
                      name="solverConfigJson"
                      value={formValues.solverConfigJson}
                      onChange={handleFieldChange}
                      spellCheck="false"
                    />
                    {errors.solverConfigJson && <strong>{errors.solverConfigJson}</strong>}
                  </>
                )}
              </label>
              <label>
                {t('replay.form.risk')}
                <select name="riskLimitsMode" value={formValues.riskLimitsMode} onChange={handleFieldChange}>
                  <option value="production">{t('replay.form.productionRisk')}</option>
                  <option value="override">{t('replay.form.overrideRisk')}</option>
                </select>
                <select name="riskLimits" value={formValues.riskLimits} onChange={handleFieldChange}>
                  <option>risk-standard</option>
                  <option>risk-tight</option>
                  <option>risk-observer</option>
                </select>
                {formValues.riskLimitsMode === 'override' && (
                  <>
                    <textarea
                      name="riskLimitsJson"
                      value={formValues.riskLimitsJson}
                      onChange={handleFieldChange}
                      spellCheck="false"
                    />
                    {errors.riskLimitsJson && <strong>{errors.riskLimitsJson}</strong>}
                  </>
                )}
              </label>
              <label>
                {t('replay.form.fee')}
                <select name="feeModelMode" value={formValues.feeModelMode} onChange={handleFieldChange}>
                  <option value="production">{t('replay.form.productionFee')}</option>
                  <option value="override">{t('replay.form.overrideFee')}</option>
                </select>
                {formValues.feeModelMode === 'override' && (
                  <>
                    <textarea
                      name="feeModelJson"
                      value={formValues.feeModelJson}
                      onChange={handleFieldChange}
                      spellCheck="false"
                    />
                    {errors.feeModelJson && <strong>{errors.feeModelJson}</strong>}
                  </>
                )}
              </label>
              <label>
                {t('replay.form.reward')}
                <select name="reward" value={formValues.reward} onChange={handleFieldChange}>
                  <option>incrementalPnL</option>
                  <option>-IS</option>
                </select>
              </label>
              <label>
                {t('replay.form.randomSeed')}
                <input name="randomSeed" value={formValues.randomSeed} onChange={handleFieldChange} />
                {errors.randomSeed && <strong>{errors.randomSeed}</strong>}
              </label>
              <label>
                {t('replay.form.tolerance')}
                <input name="tolerance" value={formValues.tolerance} onChange={handleFieldChange} />
                {errors.tolerance && <strong>{errors.tolerance}</strong>}
              </label>
            </div>

            <div className="replay-actions">
              <button type="submit" disabled={submitState.loading || !isFormSubmittable}>
                {submitState.loading ? t('replay.form.creating') : t('replay.form.run')}
              </button>
              <button type="button" onClick={loadSessions}>{t('replay.form.refresh')}</button>
            </div>
            {submitState.message && <div className="replay-inline-state tone-info-panel">{submitState.message}</div>}
            {submitState.error && <div className="replay-inline-state tone-bad-panel">{submitState.error}</div>}
          </form>

          <section className="replay-side-panel" aria-label={t('replay.sessions.title')}>
            <div className="replay-panel-header replay-sessions-header">
              <span>{t('replay.sessions.caption')}</span>
              <h2>{t('replay.sessions.title')}</h2>
            </div>

            <div className="replay-live-strip">
              <span className={`replay-live-pill ${liveState.connected ? 'connected' : ''}`}>
                {liveState.connected
                  ? t('replay.sessions.liveConnected', { transport: liveState.transport || 'stream' })
                  : t('replay.sessions.liveDisconnected')}
              </span>
              <small>{liveState.lastEventAt ? t('replay.sessions.liveUpdated', { time: formatDateTime(liveState.lastEventAt) }) : t('replay.sessions.liveWaiting')}</small>
            </div>

            <div className="replay-table-toolbar">
              <label>
                {t('replay.sessions.status')}
                <select value={statusFilter} onChange={(event) => setStatusFilter(event.target.value)}>
                  {STATUS_FILTERS.map((status) => (
                    <option key={status} value={status}>{t(`replay.statusFilter.${status}`)}</option>
                  ))}
                </select>
              </label>
              <label>
                {t('replay.sessions.search')}
                <input value={search} onChange={(event) => setSearch(event.target.value)} />
              </label>
            </div>

            {sessionsState.loading ? (
              <div className="replay-empty-state">{t('replay.states.loading')}</div>
            ) : sessionsState.error ? (
              <div className="replay-empty-state tone-bad-panel">{sessionsState.error}</div>
            ) : (
              <>
                <div className="replay-sessions-table">
                  <div className="replay-sessions-head">
                    <span>{t('replay.sessions.sessionid')}</span>
                    <span>{t('replay.sessions.status')}</span>
                    <span>{t('replay.sessions.progress')}</span>
                    <span>{t('replay.sessions.created')}</span>
                  </div>
                  {pagedSessions.map((session) => (
                    <button
                      type="button"
                      key={session.sessionid}
                      className={`replay-sessions-row ${selectedSession?.sessionid === session.sessionid ? 'selected' : ''}`}
                      onClick={() => setSelectedSessionId(session.sessionid)}
                    >
                      <span>
                        <strong>{session.name || session.sessionid}</strong>
                        <small>{session.sessionid}</small>
                      </span>
                      <span className={`replay-status replay-status-${session.status}`}>
                        {t(`replay.status.${session.status}`, { defaultValue: session.status })}
                      </span>
                      <span>
                        <i className="replay-progress-track">
                          <i style={{ width: `${Number(session.progress || 0)}%` }} />
                        </i>
                        <em>{session.progressbatches} / {session.totalbatches || '-'}</em>
                        <small>{formatNumber(session.progress, '%', 1)}</small>
                      </span>
                      <span>{formatDateTime(session.createdat)}</span>
                    </button>
                  ))}
                </div>
                <div className="replay-pagination">
                  <button type="button" disabled={currentPage === 1} onClick={() => setPage((value) => value - 1)}>
                    {t('replay.sessions.prev')}
                  </button>
                  <span>{currentPage} / {totalPages}</span>
                  <button type="button" disabled={currentPage === totalPages} onClick={() => setPage((value) => value + 1)}>
                    {t('replay.sessions.next')}
                  </button>
                </div>
              </>
            )}
          </section>
        </section>

        <section className="replay-results">
          <div className="replay-results-header">
            <div>
              <span>{selectedSession?.sessionid || '-'}</span>
              <h2>{selectedSession?.name || t('replay.results.emptyTitle')}</h2>
            </div>
            {selectedSession && (
              <div className="replay-session-actions">
                <button type="button" onClick={() => loadResults(selectedSession.sessionid)}>
                  {t('replay.results.refresh')}
                </button>
                {selectedSession.cancancel && (
                  <button
                    type="button"
                    onClick={handleCancelSession}
                    disabled={sessionActionState.loading === 'cancel'}
                  >
                    {sessionActionState.loading === 'cancel' ? t('replay.results.cancelling') : t('replay.results.cancel')}
                  </button>
                )}
                {selectedSession.canretry && (
                  <button
                    type="button"
                    onClick={handleRetrySession}
                    disabled={sessionActionState.loading === 'retry'}
                  >
                    {sessionActionState.loading === 'retry' ? t('replay.results.retrying') : t('replay.results.retry')}
                  </button>
                )}
              </div>
            )}
          </div>

          {selectedSession && (
            <div className="replay-session-facts">
              <span className={`replay-status replay-status-${selectedSession.status}`}>
                {t(`replay.status.${selectedSession.status}`, { defaultValue: selectedSession.status })}
              </span>
              <span>{selectedSession.instrument}</span>
              <span>{selectedSession.instruments?.join?.(', ') || selectedSession.symbols?.join?.(', ') || ''}</span>
              <span>{selectedSession.daterangefrom} -> {selectedSession.daterangeto}</span>
              <span>{summarizeStrategy(selectedSession.strategy)}</span>
              <span>{selectedSession.progressbatches} / {selectedSession.totalbatches || '-'}</span>
              <span>{selectedSession.solverconfigid}</span>
              <span>{selectedSession.risklimitsid}</span>
              <span>{selectedSession.sessionconfigsnapshot ? t('replay.results.snapshotUsed') : ''}</span>
            </div>
          )}

          {sessionNotice && (
            <div className={`replay-inline-state replay-session-notice tone-${sessionNotice.tone}-panel`}>
              {sessionNotice.message}
              {selectedSession?.errordetails && sessionNotice.message !== selectedSession.errordetails && (
                <small>{selectedSession.errordetails}</small>
              )}
            </div>
          )}

          {sessionActionState.message && <div className="replay-inline-state tone-info-panel">{sessionActionState.message}</div>}
          {sessionActionState.error && <div className="replay-inline-state tone-bad-panel">{sessionActionState.error}</div>}

          {resultsState.loading ? (
            <div className="replay-empty-state">{t('replay.states.resultsLoading')}</div>
          ) : resultsState.error ? (
            <div className="replay-empty-state tone-bad-panel">{resultsState.error}</div>
          ) : (
            <>
              <div className="replay-tabs" role="tablist" aria-label={t('replay.results.tabs')}>
                {RESULT_TABS.map((tab) => (
                  <button
                    type="button"
                    key={tab}
                    role="tab"
                    aria-selected={activeTab === tab}
                    className={activeTab === tab ? 'active' : ''}
                    onClick={() => setActiveTab(tab)}
                  >
                    {t(`replay.tabs.${tab}`)}
                  </button>
                ))}
              </div>

              {activeTab === 'summary' && (
                <div className="replay-summary-grid">
                  {summaryCards.map((card) => (
                    <div className="replay-summary-card" key={card.key}>
                      <span>{t(`replay.summary.${card.key}`)}</span>
                      <strong>{card.value}</strong>
                    </div>
                  ))}
                </div>
              )}

              {activeTab === 'equity' && (
                <section className="replay-equity-panel">
                  <div className="replay-section-title">
                    <h3>{t('replay.equity.title')}</h3>
                    <p>{t('replay.equity.copy')}</p>
                  </div>
                  <div className="replay-equity-chart">
                    {equityData.length > 0 ? (
                      <ResponsiveContainer width="100%" height={260}>
                        <LineChart data={equityData} margin={{ top: 20, right: 20, bottom: 10, left: 0 }}>
                          <XAxis dataKey="batchseq" stroke="#b8accd" tick={{ fill: '#b8accd', fontSize: 12 }} />
                          <YAxis stroke="#b8accd" tick={{ fill: '#b8accd', fontSize: 12 }} />
                          <Tooltip contentStyle={{ background: '#0f081c', border: '1px solid rgba(167, 74, 255, 0.3)' }} />
                          <Line type="monotone" dataKey="equity" stroke="#a74aff" strokeWidth={3} dot={false} />
                        </LineChart>
                      </ResponsiveContainer>
                    ) : (
                      <div className="replay-empty-state">{noAgentLogsMessage}</div>
                    )}
                  </div>
                </section>
              )}

              {activeTab === 'batches' && (
                <section className="replay-batches-panel">
                  <div className="replay-section-title">
                    <h3>{t('replay.batches.title')}</h3>
                    <p>{t('replay.batches.subtitle')}</p>
                  </div>
                  <div className="replay-batch-filters">
                    <input name="batchseq" value={batchFilters.batchseq} onChange={handleBatchFilterChange} placeholder={t('replay.batches.filterBatch')} />
                    <input name="pnl" value={batchFilters.pnl} onChange={handleBatchFilterChange} placeholder={t('replay.batches.filterPnl')} />
                    <input name="isvalue" value={batchFilters.isvalue} onChange={handleBatchFilterChange} placeholder={t('replay.batches.filterIs')} />
                    <input name="fillrate" value={batchFilters.fillrate} onChange={handleBatchFilterChange} placeholder={t('replay.batches.filterFillRate')} />
                    <select name="solver_error" value={batchFilters.solver_error} onChange={handleBatchFilterChange}>
                      <option value="">{t('replay.batches.filterSolverAll')}</option>
                      <option value="true">{t('replay.batches.filterSolverError')}</option>
                      <option value="false">{t('replay.batches.filterSolverOk')}</option>
                    </select>
                  </div>
                  <div className="replay-table">
                    <div className="replay-table-head">
                      <span>{t('replay.batches.batch')}</span>
                      <span>{t('replay.batches.pnl')}</span>
                      <span>{t('replay.batches.is')}</span>
                      <span>{t('replay.batches.fillRate')}</span>
                      <span>{t('replay.batches.solve')}</span>
                      <span>{t('replay.batches.residual')}</span>
                      <span>{t('replay.batches.risk')}</span>
                    </div>
                    {filteredAgentLogs.length === 0 ? (
                      <div className="replay-empty-state">{noAgentLogsMessage}</div>
                    ) : (
                      filteredAgentLogs.map((log) => (
                        <button
                          type="button"
                          className="replay-table-row replay-batch-row"
                          key={`${log.batchseq}-${log.originalbatchid || log.solvetime_ms}`}
                          onClick={() => setSelectedLog(log)}
                        >
                          <span>#{log.batchseq}</span>
                          <span>{formatPnl(log.pnl)}</span>
                          <span>{formatNumber(log.isvalue, '', 2)}</span>
                          <span>{formatNumber(log.fillrate, '%', 1)}</span>
                          <span>{formatNumber(log.solvetime_ms, ' ms', 0)}</span>
                          <span>{formatNumber(log.residualnorm, '', 6)}</span>
                          <span className={log.solvererrorflag ? 'tone-bad-text' : log.riskstatus === 'OK' ? 'tone-good-text' : 'tone-warn-text'}>
                            {log.solvererrorflag ? t('replay.batches.solverError') : log.riskstatus || '-'}
                          </span>
                        </button>
                      ))
                    )}
                  </div>
                  <div className="replay-pagination">
                    <button
                      type="button"
                      disabled={currentBatchPage === 1}
                      onClick={() => setBatchPage((value) => Math.max(1, value - 1))}
                    >
                      {t('replay.batches.prev')}
                    </button>
                    <span>{currentBatchPage} / {batchTotalPages}</span>
                    <button
                      type="button"
                      disabled={currentBatchPage === batchTotalPages}
                      onClick={() => setBatchPage((value) => value + 1)}
                    >
                      {t('replay.batches.next')}
                    </button>
                  </div>
                  {selectedLog && (
                    <div className="replay-log-details" role="dialog" aria-label={t('replay.batches.details')}>
                      <div className="replay-log-details-header">
                        <h4>{t('replay.batches.details')} #{selectedLog.batchseq}</h4>
                        <button type="button" onClick={() => setSelectedLog(null)}>{t('replay.batches.closeDetails')}</button>
                      </div>
                      <dl>
                        <dt>originalbatchid</dt><dd>{selectedLog.originalbatchid || '-'}</dd>
                        <dt>reward</dt><dd>{formatNumber(selectedLog.reward, '', 6)}</dd>
                        <dt>pnl</dt><dd>{formatPnl(selectedLog.pnl)}</dd>
                        <dt>is_value</dt><dd>{formatNumber(selectedLog.isvalue, '', 6)}</dd>
                        <dt>fillrate</dt><dd>{formatNumber(selectedLog.fillrate, '%', 2)}</dd>
                        <dt>solvetime_ms</dt><dd>{formatNumber(selectedLog.solvetime_ms, ' ms', 0)}</dd>
                        <dt>residualnorm</dt><dd>{formatNumber(selectedLog.residualnorm, '', 6)}</dd>
                        <dt>riskstatus</dt><dd>{selectedLog.riskstatus || '-'}</dd>
                        <dt>solvererrorflag</dt><dd>{String(Boolean(selectedLog.solvererrorflag))}</dd>
                        <dt>errorcode</dt><dd>{selectedLog.errorcode || '-'}</dd>
                        <dt>sessionconfigsnapshotversion</dt><dd>{selectedLog.sessionconfigsnapshotversion || '-'}</dd>
                      </dl>
                      <pre>{JSON.stringify({
                        state: selectedLog.state,
                        action: selectedLog.action,
                        fills: selectedLog.fills,
                        batchresult: selectedLog.batchresult,
                      }, null, 2)}</pre>
                    </div>
                  )}
                </section>
              )}

              {activeTab === 'compare' && (
              <section className="replay-audit-panel">
                <div className="replay-section-title">
                  <h3>{t('replay.compare.title')}</h3>
                  <p>{t('replay.compare.subtitle')}</p>
                </div>

                <form className="replay-compare-form" onSubmit={handleRunCompare}>
                  <label>
                    {t('replay.compare.sessionA')}
                    <select name="sessionA" value={compareSelection.sessionA} onChange={handleCompareChange}>
                      {sessions.map((session) => (
                        <option key={`A-${session.sessionid}`} value={session.sessionid}>
                          {session.name || session.sessionid}
                        </option>
                      ))}
                    </select>
                  </label>
                  <label>
                    {t('replay.compare.sessionB')}
                    <select name="sessionB" value={compareSelection.sessionB} onChange={handleCompareChange}>
                      {sessions.map((session) => (
                        <option key={`B-${session.sessionid}`} value={session.sessionid}>
                          {session.name || session.sessionid}
                        </option>
                      ))}
                    </select>
                  </label>
                  <button type="submit" disabled={compareState.loading}>
                    {compareState.loading ? t('replay.compare.running') : t('replay.compare.run')}
                  </button>
                </form>

                {compareState.error && <div className="replay-empty-state tone-bad-panel">{compareState.error}</div>}
                {compareState.message && <div className="replay-inline-state tone-info-panel">{compareState.message}</div>}

                {compareRows.length > 0 && (
                  <div className="replay-compare-table">
                    <div className="replay-table-head replay-compare-head">
                      <span>{t('replay.compare.table.metric')}</span>
                      <span>{sessionNameById[compareResult?.sessionA] || compareResult?.sessionA || t('replay.compare.sessionA')}</span>
                      <span>{sessionNameById[compareResult?.sessionB] || compareResult?.sessionB || t('replay.compare.sessionB')}</span>
                      <span>{t('replay.compare.table.delta')}</span>
                      <span>{t('replay.compare.table.verdict')}</span>
                    </div>
                    {compareRows.map((metric) => (
                      <div className="replay-table-row replay-compare-row" key={metric.key}>
                        <span>{t(`replay.compare.metrics.${metric.key}`, { defaultValue: metric.label || metric.key })}</span>
                        <span>{formatCompareMetricValue(metric.key, metric.valueA)}</span>
                        <span>{formatCompareMetricValue(metric.key, metric.valueB)}</span>
                        <span>{formatCompareDelta(metric.key, metric.delta)}</span>
                        <span
                          className={
                            metric.better === 'equal'
                              ? ''
                              : 'tone-good-text'
                          }
                        >
                          {metric.better === 'equal'
                            ? t('replay.compare.verdict.equal')
                            : metric.better === 'A'
                              ? t('replay.compare.verdict.aBetter')
                              : t('replay.compare.verdict.bBetter')}
                        </span>
                      </div>
                    ))}
                  </div>
                )}
              </section>
              )}

              {activeTab === 'audit' && (
              <section className="replay-audit-panel">
                <div className="replay-section-title">
                  <h3>{t('replay.audit.title')}</h3>
                  <p>{t('replay.audit.subtitle')}</p>
                </div>

                <form className="replay-audit-form" onSubmit={handleRunAudit}>
                  <label>
                    {t('replay.audit.batchId')}
                    <input
                      value={auditBatchId}
                      onChange={(event) => {
                        setAuditBatchId(event.target.value);
                        if (auditState.error) {
                          setAuditState((current) => ({ ...current, error: '' }));
                        }
                      }}
                      placeholder={t('replay.audit.batchIdPlaceholder')}
                    />
                  </label>
                  <button type="submit" disabled={auditState.loading}>
                    {auditState.loading ? t('replay.audit.running') : t('replay.audit.run')}
                  </button>
                </form>

                {auditState.error && <div className="replay-empty-state tone-bad-panel">{auditState.error}</div>}
                {auditState.message && <div className="replay-inline-state tone-info-panel">{auditState.message}</div>}

                {auditDiffs.length > 0 && (
                  <div className="replay-audit-layout">
                    <div className="replay-audit-list">
                      <div className="replay-table-head replay-audit-head">
                        <span>{t('replay.audit.table.batch')}</span>
                        <span>{t('replay.audit.table.equivalent')}</span>
                        <span>{t('replay.audit.table.residualDelta')}</span>
                      </div>
                      {auditDiffs.map((diff) => (
                        <button
                          type="button"
                          key={diff.batchid}
                          className={`replay-audit-row ${selectedAuditDiff?.batchid === diff.batchid ? 'selected' : ''}`}
                          onClick={() => setSelectedAuditBatchId(diff.batchid)}
                        >
                          <span>#{diff.batchid}</span>
                          <span className={diff.equivalent ? 'tone-good-text' : 'tone-warn-text'}>
                            {diff.equivalent ? t('replay.audit.equivalent') : t('replay.audit.different')}
                          </span>
                          <span>{formatSigned(diff?.residualnorm?.delta, 6)}</span>
                        </button>
                      ))}
                    </div>

                    {selectedAuditDiff && (
                      <div className="replay-audit-details">
                        <div className="replay-audit-summary">
                          <div>
                            <span>{t('replay.audit.detail.residual')}</span>
                            <strong>{formatNumber(selectedAuditDiff?.residualnorm?.replay, '', 6)}</strong>
                            <small>
                              {t('replay.audit.detail.production')}: {formatNumber(selectedAuditDiff?.residualnorm?.production, '', 6)}
                            </small>
                          </div>
                          <div>
                            <span>{t('replay.audit.detail.riskStatus')}</span>
                            <strong>{selectedAuditDiff?.riskstatus?.replay || '-'}</strong>
                            <small>
                              {t('replay.audit.detail.production')}: {selectedAuditDiff?.riskstatus?.production || '-'}
                            </small>
                          </div>
                          <div>
                            <span>{t('replay.audit.detail.equivalent')}</span>
                            <strong className={selectedAuditDiff.equivalent ? 'tone-good-text' : 'tone-warn-text'}>
                              {selectedAuditDiff.equivalent ? t('replay.audit.equivalent') : t('replay.audit.different')}
                            </strong>
                            <small>{t('replay.audit.detail.batch')}: {selectedAuditDiff.batchid}</small>
                          </div>
                        </div>

                        <div className="replay-audit-diff-grid">
                          <section className="replay-audit-table">
                            <h4>{t('replay.audit.detail.clearPrices')}</h4>
                            <div className="replay-table">
                              <div className="replay-table-head replay-audit-clear-head">
                                <span>{t('replay.audit.detail.symbol')}</span>
                                <span>{t('replay.audit.detail.replay')}</span>
                                <span>{t('replay.audit.detail.production')}</span>
                                <span>{t('replay.audit.detail.delta')}</span>
                              </div>
                              {(selectedAuditDiff.clearprices || []).map((row) => (
                                <div className="replay-table-row replay-audit-clear-row" key={`${row.symbol}-${row.delta}`}>
                                  <span>{row.symbol}</span>
                                  <span>{formatNumber(row.replay, '', 4)}</span>
                                  <span>{formatNumber(row.production, '', 4)}</span>
                                  <span>{formatSigned(row.delta, 4)}</span>
                                </div>
                              ))}
                            </div>
                          </section>

                          <section className="replay-audit-table">
                            <h4>{t('replay.audit.detail.fills')}</h4>
                            <div className="replay-table">
                              <div className="replay-table-head replay-audit-fills-head">
                                <span>{t('replay.audit.detail.fill')}</span>
                                <span>{t('replay.audit.detail.replay')}</span>
                                <span>{t('replay.audit.detail.production')}</span>
                                <span>{t('replay.audit.detail.delta')}</span>
                              </div>
                              {(selectedAuditDiff.fills || []).map((row) => (
                                <div className="replay-table-row replay-audit-fills-row" key={row.fillid}>
                                  <span>{row.fillid}</span>
                                  <span>{formatNumber(row.replayprice, '', 4)} / {formatNumber(row.replayqty, '', 4)}</span>
                                  <span>{formatNumber(row.productionprice, '', 4)} / {formatNumber(row.productionqty, '', 4)}</span>
                                  <span>{formatSigned(row.pricedelta, 4)} / {formatSigned(row.qtydelta, 4)}</span>
                                </div>
                              ))}
                            </div>
                          </section>
                        </div>

                        {selectedAuditDiff.productionrecordurl && (
                          <a
                            className="replay-audit-production-link"
                            href={selectedAuditDiff.productionrecordurl}
                            target="_blank"
                            rel="noreferrer"
                          >
                            {t('replay.audit.detail.openProduction')}
                          </a>
                        )}
                      </div>
                    )}
                  </div>
                )}
              </section>
              )}
            </>
          )}
        </section>
      </main>
    </div>
  );
};

export default BacktestReplay;
