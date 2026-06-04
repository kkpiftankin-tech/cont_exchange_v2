import React from 'react';
import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import BacktestReplay from '../BacktestReplay';

const mockListReplaySessions = jest.fn();
const mockCreateReplaySession = jest.fn();
const mockGetReplaySummary = jest.fn();
const mockGetReplayAgentLogs = jest.fn();
const mockGetReplayAuditDiff = jest.fn();
const mockGetReplayCompare = jest.fn();
const mockCancelReplaySession = jest.fn();
const mockRetryReplaySession = jest.fn();
const mockSubscribeReplayResults = jest.fn();
const mockGetReplayErrorMeta = jest.fn();
const mockLogout = jest.fn();
const mockNavigate = jest.fn();
const translate = (key, options = {}) => {
  if (options.defaultValue) return options.defaultValue;
  if (key === 'replay.sessions.liveConnected') return `live:${options.transport}`;
  if (key === 'replay.sessions.liveUpdated') return `updated:${options.time}`;
  if (key === 'replay.audit.loaded') return `audit:${options.count}`;
  if (key === 'replay.playback.batchOf') return `batch:${options.current}/${options.total}`;
  if (key === 'replay.playback.buffering') return `buffering:${options.loaded}/${options.total}`;
  if (key === 'replay.playback.cappedNotice') return `capped:${options.cap}/${options.total}`;
  return key;
};

let lastSubscription = null;

jest.mock('../../../api/replayService', () => ({
  listReplaySessions: (...args) => mockListReplaySessions(...args),
  createReplaySession: (...args) => mockCreateReplaySession(...args),
  getReplaySummary: (...args) => mockGetReplaySummary(...args),
  getReplayAgentLogs: (...args) => mockGetReplayAgentLogs(...args),
  getReplayAuditDiff: (...args) => mockGetReplayAuditDiff(...args),
  getReplayCompare: (...args) => mockGetReplayCompare(...args),
  cancelReplaySession: (...args) => mockCancelReplaySession(...args),
  retryReplaySession: (...args) => mockRetryReplaySession(...args),
  subscribeReplayResults: (...args) => mockSubscribeReplayResults(...args),
  getReplayErrorMeta: (...args) => mockGetReplayErrorMeta(...args),
}));

jest.mock('../../../api/authService', () => ({
  logout: () => mockLogout(),
}));

jest.mock('react-router-dom', () => ({
  useNavigate: () => mockNavigate,
}), { virtual: true });

jest.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: translate,
  }),
}));

jest.mock('recharts', () => {
  const React = require('react');
  return {
    ResponsiveContainer: ({ children }) => <div data-testid="chart">{children}</div>,
    LineChart: ({ children }) => <div>{children}</div>,
    Line: () => <div />,
    XAxis: () => <div />,
    YAxis: () => <div />,
    Tooltip: () => <div />,
  };
});

function makeSession(overrides = {}) {
  return {
    sessionid: 'rpl-test-1',
    name: 'Replay Session',
    status: 'pending',
    progressbatches: 0,
    totalbatches: 10,
    progress: 0,
    instrument: 'BTC/USDT',
    daterangefrom: '2026-04-01',
    daterangeto: '2026-04-02',
    solverconfigid: 'solver-prod-v4',
    cancancel: true,
    canretry: false,
    partialsummary: false,
    errorcode: '',
    errordetails: '',
    createdat: '2026-04-26T12:00:00.000Z',
    ...overrides,
  };
}

function renderReplayPage() {
  return render(<BacktestReplay />);
}

describe('BacktestReplay UI', () => {
  beforeEach(() => {
    jest.clearAllMocks();
    lastSubscription = null;
    mockGetReplayErrorMeta.mockReturnValue({ status: 0, code: '', message: '' });
    mockCreateReplaySession.mockResolvedValue({ session: makeSession() });
    mockGetReplayAuditDiff.mockResolvedValue({ items: [], total: 0 });
    mockGetReplayCompare.mockResolvedValue({ compatible: true, metrics: [] });
    mockCancelReplaySession.mockResolvedValue(makeSession({ status: 'cancelled', cancancel: false, canretry: true }));
    mockRetryReplaySession.mockResolvedValue({ session: makeSession({ sessionid: 'rpl-retry-1', name: 'Replay retry', status: 'pending' }) });
    mockSubscribeReplayResults.mockImplementation((callbacks) => {
      lastSubscription = callbacks;
      callbacks.onOpen?.({ transport: 'sse' });
      return jest.fn();
    });
  });

  test('updates live progress from pending to completed', async () => {
    let phase = 'pending';
    const sessionByPhase = {
      pending: makeSession(),
      running: makeSession({ status: 'running', progressbatches: 4, totalbatches: 10, progress: 40, cancancel: true }),
      completed: makeSession({ status: 'completed', progressbatches: 10, totalbatches: 10, progress: 100, cancancel: false, partialsummary: false }),
    };

    mockListReplaySessions.mockImplementation(async () => ({ items: [sessionByPhase[phase]], total: 1 }));
    mockGetReplaySummary.mockImplementation(async () => ({
      sessionid: 'rpl-test-1',
      processedbatches: sessionByPhase[phase].progressbatches,
      totalbatches: 10,
      partial: false,
      totalpnl: phase === 'completed' ? 150 : 0,
      avgis: -2,
      sharpe: 1.1,
      fillrate: phase === 'completed' ? 92 : 0,
      avgvwap: 68123.4,
      maxdrawdown: 1.2,
      avgsolvetime: 31,
    }));
    mockGetReplayAgentLogs.mockImplementation(async () => ({
      items: phase === 'completed'
        ? [{ batchseq: 10, pnl: 150, isvalue: -2, fillrate: 92, solvetime_ms: 31, riskstatus: 'OK', solvererrorflag: false }]
        : [],
      total: phase === 'completed' ? 1 : 0,
    }));

    renderReplayPage();

    expect(await screen.findByText('replay.states.pending')).toBeInTheDocument();
    expect(screen.getAllByText('0 / 10').length).toBeGreaterThan(0);
    expect(screen.getByText('live:sse')).toBeInTheDocument();

    phase = 'running';
    await act(async () => {
      lastSubscription.onEvent({
        type: 'replay.progress',
        sessionid: 'rpl-test-1',
        transport: 'sse',
        session: sessionByPhase.running,
      });
    });

    expect(await screen.findByText('replay.states.liveRunning')).toBeInTheDocument();
    expect(screen.getAllByText('4 / 10').length).toBeGreaterThan(0);

    phase = 'completed';
    await act(async () => {
      lastSubscription.onEvent({
        type: 'replay.completed',
        sessionid: 'rpl-test-1',
        transport: 'sse',
        session: sessionByPhase.completed,
        summary: await mockGetReplaySummary(),
      });
    });

    expect((await screen.findAllByText('10 / 10')).length).toBeGreaterThan(0);
    fireEvent.click(screen.getByRole('tab', { name: 'replay.tabs.batches' }));
    expect(await screen.findByText('#10')).toBeInTheDocument();
  });

  test('shows solver failure state and supports retry', async () => {
    mockListReplaySessions.mockResolvedValue({
      items: [
        makeSession({
          sessionid: 'rpl-failed-1',
          name: 'Failed Session',
          status: 'failed',
          progressbatches: 5,
          totalbatches: 10,
          progress: 50,
          cancancel: false,
          canretry: true,
          errorcode: 'solver_error',
          errordetails: 'Solver diverged at batch 5.',
        }),
      ],
      total: 1,
    });
    mockGetReplaySummary.mockResolvedValue({
      sessionid: 'rpl-failed-1',
      processedbatches: 5,
      totalbatches: 10,
      partial: true,
      totalpnl: -40,
      avgis: -7,
      sharpe: -0.3,
      fillrate: 66,
      avgvwap: 3521.5,
      maxdrawdown: 4.2,
      avgsolvetime: 71,
    });
    mockGetReplayAgentLogs.mockResolvedValue({
      items: [{ batchseq: 5, pnl: -40, isvalue: -7, fillrate: 66, solvetime_ms: 71, riskstatus: 'HARD_ALERT', solvererrorflag: true }],
      total: 1,
    });
    mockRetryReplaySession.mockResolvedValue({
      session: makeSession({
        sessionid: 'rpl-retry-42',
        name: 'Failed Session retry',
        status: 'pending',
        cancancel: true,
        canretry: false,
      }),
    });

    renderReplayPage();

    expect(await screen.findByText('Solver diverged at batch 5.')).toBeInTheDocument();
    fireEvent.click(await screen.findByRole('button', { name: 'replay.results.retry' }));

    await waitFor(() => {
      expect(mockRetryReplaySession).toHaveBeenCalledWith('rpl-failed-1');
    });

    expect(await screen.findByText('replay.states.retryStarted')).toBeInTheDocument();
    expect((await screen.findAllByText('rpl-retry-42')).length).toBeGreaterThan(0);
  });

  test('cancels running session and keeps partial results accessible', async () => {
    let currentSession = makeSession({
      sessionid: 'rpl-running-1',
      name: 'Running Session',
      status: 'running',
      progressbatches: 6,
      totalbatches: 10,
      progress: 60,
      cancancel: true,
      partialsummary: true,
    });

    mockListReplaySessions.mockImplementation(async () => ({ items: [currentSession], total: 1 }));
    mockGetReplaySummary.mockImplementation(async () => ({
      sessionid: 'rpl-running-1',
      processedbatches: currentSession.progressbatches,
      totalbatches: 10,
      partial: currentSession.status !== 'completed',
      totalpnl: 24,
      avgis: -3,
      sharpe: 0.7,
      fillrate: 73,
      avgvwap: 68002.1,
      maxdrawdown: 2.3,
      avgsolvetime: 34,
    }));
    mockGetReplayAgentLogs.mockResolvedValue({
      items: [{ batchseq: 6, pnl: 24, isvalue: -3, fillrate: 73, solvetime_ms: 34, riskstatus: 'WARN', solvererrorflag: false }],
      total: 1,
    });
    mockCancelReplaySession.mockImplementation(async () => {
      currentSession = makeSession({
        sessionid: 'rpl-running-1',
        name: 'Running Session',
        status: 'cancelled',
        progressbatches: 6,
        totalbatches: 10,
        progress: 60,
        cancancel: false,
        canretry: true,
        partialsummary: true,
        errorcode: 'cancelled_by_user',
        errordetails: 'Replay was cancelled from the UI.',
      });
      return currentSession;
    });

    renderReplayPage();

    fireEvent.click(await screen.findByRole('button', { name: 'replay.results.cancel' }));

    await waitFor(() => {
      expect(mockCancelReplaySession).toHaveBeenCalledWith('rpl-running-1');
    });

    expect(await screen.findByText('replay.states.cancelled')).toBeInTheDocument();
    expect(await screen.findByText('replay.states.cancelledPartial')).toBeInTheDocument();
    expect(screen.getByText('Replay was cancelled from the UI.')).toBeInTheDocument();
  });

  test('loads compare and audit data from UI actions', async () => {
    mockListReplaySessions.mockResolvedValue({
      items: [
        makeSession({ sessionid: 'rpl-a', name: 'Session A', status: 'completed', progressbatches: 10, totalbatches: 10, progress: 100, cancancel: false, partialsummary: false }),
        makeSession({ sessionid: 'rpl-b', name: 'Session B', status: 'completed', progressbatches: 10, totalbatches: 10, progress: 100, cancancel: false, partialsummary: false, createdat: '2026-04-25T12:00:00.000Z' }),
      ],
      total: 2,
    });
    mockGetReplaySummary.mockResolvedValue({
      sessionid: 'rpl-a',
      processedbatches: 10,
      totalbatches: 10,
      partial: false,
      totalpnl: 200,
      avgis: -2,
      sharpe: 1.6,
      fillrate: 94,
      avgvwap: 68120,
      maxdrawdown: 1.1,
      avgsolvetime: 28,
    });
    mockGetReplayAgentLogs.mockResolvedValue({
      items: [{ batchseq: 10, pnl: 200, isvalue: -2, fillrate: 94, solvetime_ms: 28, riskstatus: 'OK', solvererrorflag: false }],
      total: 1,
    });
    mockGetReplayCompare.mockResolvedValue({
      compatible: true,
      sessionA: 'rpl-a',
      sessionB: 'rpl-b',
      metrics: [
        { key: 'sharpe', label: 'Sharpe', valueA: 1.1, valueB: 1.4, delta: 0.3, better: 'B' },
        { key: 'avgpnl', label: 'Avg PnL', valueA: 20, valueB: 25, delta: 5, better: 'B' },
        { key: 'stdpnl', label: 'Std PnL', valueA: 7, valueB: 5, delta: -2, better: 'B' },
        { key: 'avgvwap', label: 'Avg VWAP', valueA: 60100, valueB: 60125, delta: 25, better: 'equal', preference: 'neutral' },
      ],
    });
    renderReplayPage();

    await waitFor(() => {
      expect(screen.getAllByText('Session A').length).toBeGreaterThan(0);
    });
    fireEvent.click(screen.getByRole('tab', { name: 'replay.tabs.compare' }));
    expect(screen.getByLabelText('replay.compare.sessionA').value).toBe('rpl-a');
    expect(screen.getByLabelText('replay.compare.sessionB').value).toBe('rpl-b');
    fireEvent.click(await screen.findByRole('button', { name: 'replay.compare.run' }));
    await waitFor(() => {
      expect(mockGetReplayCompare).toHaveBeenCalledWith({ sessionA: 'rpl-a', sessionB: 'rpl-b' });
    });
    expect(await screen.findByText('Sharpe')).toBeInTheDocument();
    expect(await screen.findByText('Avg PnL')).toBeInTheDocument();
    expect(await screen.findByText('Std PnL')).toBeInTheDocument();
    expect(await screen.findByText('Avg VWAP')).toBeInTheDocument();
    expect(screen.queryByRole('tab', { name: 'replay.tabs.audit' })).not.toBeInTheDocument();
  });

  test('shows permission denied when replay creation is rejected', async () => {
    mockListReplaySessions.mockResolvedValue({ items: [makeSession()], total: 1 });
    mockGetReplaySummary.mockResolvedValue({
      sessionid: 'rpl-test-1',
      processedbatches: 0,
      totalbatches: 10,
      partial: true,
      totalpnl: 0,
      avgis: 0,
      sharpe: 0,
      fillrate: 0,
      avgvwap: 0,
      maxdrawdown: 0,
      avgsolvetime: 0,
    });
    mockGetReplayAgentLogs.mockResolvedValue({ items: [], total: 0 });
    mockCreateReplaySession.mockRejectedValue(new Error('forbidden'));
    mockGetReplayErrorMeta.mockReturnValue({ status: 403, code: 'permission_denied', message: 'Forbidden' });

    renderReplayPage();

    fireEvent.click(await screen.findByRole('button', { name: 'replay.form.run' }));

    expect(await screen.findByText('replay.states.permissionDenied')).toBeInTheDocument();
  });

  test('submits F-15 payload and disables run for invalid form data', async () => {
    mockListReplaySessions.mockResolvedValue({ items: [makeSession()], total: 1 });
    mockGetReplaySummary.mockResolvedValue({
      sessionid: 'rpl-test-1',
      processedbatches: 0,
      totalbatches: 10,
      partial: true,
    });
    mockGetReplayAgentLogs.mockResolvedValue({ items: [], total: 0 });

    const { container } = renderReplayPage();
    const runButton = await screen.findByRole('button', { name: 'replay.form.run' });
    expect(runButton).toBeEnabled();

    fireEvent.click(runButton);
    await waitFor(() => {
      expect(mockCreateReplaySession).toHaveBeenCalled();
    });
    expect(mockCreateReplaySession.mock.calls[0][0]).toMatchObject({
      name: 'BTC replay validation',
      instruments: ['BTCUSDT'],
      daterangefrom: '2026-05-25',
      daterangeto: '2026-05-26',
      solverconfigid: 'solver-prod-v4',
      risklimitsid: 'risk-standard',
      feemodel: { production: true },
      rewardmode: 'incrementalPnL',
    });
    expect(mockCreateReplaySession.mock.calls[0][0].strategy[0]).toMatchObject({
      symbol: 'BTCUSDT',
      side: 'buy',
      pL: 58000,
      pH: 62000,
      qrate: 0.5,
      qmax: 100,
      executionwindow: 3600,
    });

    const strategyEditor = screen.getByLabelText('replay.form.strategy');
    fireEvent.change(strategyEditor, {
      target: {
        value: '[{"symbol":"BTCUSDT","side":"buy","pL":62000,"pH":58000,"qrate":0.5,"qmax":100}]',
      },
    });
    expect(runButton).toBeDisabled();

    fireEvent.change(strategyEditor, {
      target: {
        value: '[{"symbol":"BTCUSDT","side":"buy","pL":58000,"pH":62000,"qrate":0.5,"qmax":100}]',
      },
    });
    fireEvent.change(container.querySelector('select[name="solverConfigMode"]'), {
      target: { name: 'solverConfigMode', value: 'override' },
    });
    fireEvent.change(container.querySelector('textarea[name="solverConfigJson"]'), {
      target: { name: 'solverConfigJson', value: '{"batchintervalms":1000}' },
    });
    expect(runButton).toBeDisabled();
  });

  test('persist toggle is OFF by default', async () => {
    mockListReplaySessions.mockResolvedValue({ items: [], total: 0 });
    mockGetReplaySummary.mockResolvedValue({});
    mockGetReplayAgentLogs.mockResolvedValue({ items: [], total: 0 });

    const { container } = renderReplayPage();
    await waitFor(() => {
      expect(mockListReplaySessions).toHaveBeenCalled();
    });

    const persistCheckbox = container.querySelector('input[name="persist"]');
    expect(persistCheckbox).toBeInTheDocument();
    expect(persistCheckbox.checked).toBe(false);
  });

  test('payload contains persist:false by default and persist:true when checked', async () => {
    mockListReplaySessions.mockResolvedValue({ items: [], total: 0 });
    mockGetReplaySummary.mockResolvedValue({});
    mockGetReplayAgentLogs.mockResolvedValue({ items: [], total: 0 });

    const { container } = renderReplayPage();
    await waitFor(() => {
      expect(mockListReplaySessions).toHaveBeenCalled();
    });

    // Default: persist=false => ephemeral create
    fireEvent.click(await screen.findByRole('button', { name: 'replay.form.run' }));
    await waitFor(() => {
      expect(mockCreateReplaySession).toHaveBeenCalled();
    });
    expect(mockCreateReplaySession.mock.calls[0][0]).toMatchObject({ persist: false });

    mockCreateReplaySession.mockClear();

    // Check the persist checkbox => persist:true
    const persistCheckbox = container.querySelector('input[name="persist"]');
    fireEvent.click(persistCheckbox);
    expect(persistCheckbox.checked).toBe(true);

    fireEvent.click(screen.getByRole('button', { name: 'replay.form.run' }));
    await waitFor(() => {
      expect(mockCreateReplaySession).toHaveBeenCalled();
    });
    expect(mockCreateReplaySession.mock.calls[0][0]).toMatchObject({ persist: true });
  });

  test('ephemeral run does not add to sessions list and renders summary from stream event', async () => {
    mockListReplaySessions.mockResolvedValue({ items: [], total: 0 });
    mockGetReplaySummary.mockResolvedValue({});
    mockGetReplayAgentLogs.mockResolvedValue({ items: [], total: 0 });

    const ephemeralCreatedSession = makeSession({
      sessionid: 'eph-001',
      name: 'Ephemeral run',
      status: 'running',
    });
    mockCreateReplaySession.mockResolvedValue({ session: ephemeralCreatedSession });

    let ephemeralSubscription = null;
    mockSubscribeReplayResults.mockImplementation((callbacks) => {
      // Track only the ephemeral subscription (sessionid = eph-001)
      if (callbacks.sessionid === 'eph-001') {
        ephemeralSubscription = callbacks;
      } else {
        lastSubscription = callbacks;
        callbacks.onOpen?.({ transport: 'sse' });
      }
      return jest.fn();
    });

    renderReplayPage();
    await waitFor(() => {
      expect(mockListReplaySessions).toHaveBeenCalled();
    });

    // persist is OFF by default: run the form
    fireEvent.click(await screen.findByRole('button', { name: 'replay.form.run' }));
    await waitFor(() => {
      expect(mockCreateReplaySession).toHaveBeenCalled();
    });

    // The session must NOT appear in the sessions list
    expect(screen.queryByText('eph-001')).not.toBeInTheDocument();

    // The ephemeral panel should appear
    expect(await screen.findByText('replay.ephemeral.panelTitle')).toBeInTheDocument();
    expect(screen.getAllByText('replay.states.ephemeralStarted').length).toBeGreaterThan(0);

    // Simulate stream completion event with summary
    const streamSummary = {
      totalpnl: 42.5,
      avgis: -1.1,
      sharpe: 1.3,
      fillrate: 88,
      maxdrawdown: 0.5,
      avgsolvetime: 25,
      totalbatches: 10,
      totalfillevents: 55,
      processedbatches: 10,
      failedbatches: 0,
      partial: false,
      nodata: false,
    };
    await act(async () => {
      ephemeralSubscription?.onEvent({
        type: 'replay.completed',
        sessionid: 'eph-001',
        status: 'completed',
        summary: streamSummary,
      });
    });

    expect(await screen.findByText('replay.states.ephemeralCompleted')).toBeInTheDocument();
    // No REST call for loadResults (getReplaySummary should NOT be called for eph-001)
    expect(mockGetReplaySummary).not.toHaveBeenCalledWith('eph-001');

    // Dismiss button closes the panel
    fireEvent.click(screen.getByRole('button', { name: 'replay.ephemeral.dismiss' }));
    expect(screen.queryByText('replay.ephemeral.panelTitle')).not.toBeInTheDocument();
  });

  test('requests AgentLog pagination and filters from backend', async () => {
    mockListReplaySessions.mockResolvedValue({ items: [makeSession({ status: 'completed', cancancel: false })], total: 1 });
    mockGetReplaySummary.mockResolvedValue({
      sessionid: 'rpl-test-1',
      processedbatches: 25,
      totalbatches: 25,
      partial: false,
    });
    mockGetReplayAgentLogs.mockResolvedValue({
      items: [{
        batchseq: 1,
        originalbatchid: 'batch-1',
        pnl: 1.25,
        isvalue: 0.5,
        fillrate: 100,
        solvetime_ms: 10,
        residualnorm: 0.00012,
        reward: 1.25,
        riskstatus: 'OK',
        solvererrorflag: false,
        errorcode: '',
        state: { mid: 60050 },
        action: [{ symbol: 'BTCUSDT', qmax: 100 }],
        fills: [{ fillid: 'fill-1', execqty: 2 }],
        batchresult: { originalbatchid: 'batch-1', residualnorm: 0.00012 },
      }],
      total: 25,
    });

    renderReplayPage();
    fireEvent.click(await screen.findByRole('tab', { name: 'replay.tabs.batches' }));
    expect(await screen.findByText('#1')).toBeInTheDocument();

    await waitFor(() => {
      expect(mockGetReplayAgentLogs).toHaveBeenCalledWith('rpl-test-1', expect.objectContaining({
        limit: 20,
        offset: 0,
      }));
    });

    fireEvent.click(screen.getByRole('button', { name: 'replay.batches.next' }));
    expect(await screen.findByText('#1')).toBeInTheDocument();
    await waitFor(() => {
      expect(mockGetReplayAgentLogs).toHaveBeenCalledWith('rpl-test-1', expect.objectContaining({
        limit: 20,
        offset: 20,
      }));
    });

    fireEvent.click(await screen.findByText('#1'));
    expect(await screen.findByText('originalbatchid')).toBeInTheDocument();
    expect(screen.getAllByText('batch-1').length).toBeGreaterThan(0);
    expect(screen.getByText('pnl')).toBeInTheDocument();
    expect(screen.getAllByText('+1.25 USDT').length).toBeGreaterThan(0);
    expect(screen.getByText('is_value')).toBeInTheDocument();
    expect(screen.getByText('residualnorm')).toBeInTheDocument();
    expect(screen.getByText(/fill-1/)).toBeInTheDocument();

    fireEvent.change(screen.getByPlaceholderText('replay.batches.filterBatch'), {
      target: { name: 'batchseq', value: '5' },
    });
    await waitFor(() => {
      expect(mockGetReplayAgentLogs).toHaveBeenCalledWith('rpl-test-1', expect.objectContaining({
        batchseq: '5',
        offset: 0,
      }));
    });
  });

  // ===== Playback tests =====

  test('playback tab appears only for completed sessions and advances frames on Play', async () => {
    jest.useFakeTimers();

    const completedSession = makeSession({
      sessionid: 'rpl-pb-1',
      name: 'Playback Session',
      status: 'completed',
      progressbatches: 3,
      totalbatches: 3,
      progress: 100,
      cancancel: false,
      canretry: false,
    });

    mockListReplaySessions.mockResolvedValue({ items: [completedSession], total: 1 });
    mockGetReplaySummary.mockResolvedValue({ sessionid: 'rpl-pb-1', totalpnl: 10 });
    mockGetReplayAgentLogs.mockImplementation(async (_sessionid, opts) => {
      if (opts && opts.limit === 200) {
        // Playback buffer fetch
        return {
          items: [
            { batchseq: 1, pnl: 1, fillrate: 80, solvetime_ms: 10, residualnorm: 0.001, riskstatus: 'OK', solvererrorflag: false, batchresult: { clearprices: { BTCUSDT: 60000 } } },
            { batchseq: 2, pnl: 2, fillrate: 85, solvetime_ms: 12, residualnorm: 0.001, riskstatus: 'OK', solvererrorflag: false, batchresult: { clearprices: { BTCUSDT: 60100 } } },
            { batchseq: 3, pnl: 3, fillrate: 90, solvetime_ms: 15, residualnorm: 0.001, riskstatus: 'OK', solvererrorflag: false, batchresult: { clearprices: { BTCUSDT: 60200 } } },
          ],
          total: 3,
        };
      }
      // Normal batch page fetch
      return { items: [], total: 0 };
    });

    renderReplayPage();

    // Wait for sessions to load
    await waitFor(() => expect(mockListReplaySessions).toHaveBeenCalled());

    // Playback tab should be present for completed session
    const playbackTab = await screen.findByRole('tab', { name: 'replay.tabs.playback' });
    expect(playbackTab).toBeInTheDocument();

    // Open Playback tab — triggers buffer load
    await act(async () => {
      fireEvent.click(playbackTab);
    });

    // Wait for buffer to finish loading (status goes idle)
    await waitFor(() => {
      expect(mockGetReplayAgentLogs).toHaveBeenCalledWith(
        'rpl-pb-1',
        expect.objectContaining({ limit: 200, offset: 0 })
      );
    });

    // Play button should be visible
    const playBtn = await screen.findByRole('button', { name: 'replay.playback.play' });
    expect(playBtn).toBeInTheDocument();

    // batch:1/3 should be shown (cursor=0 means first batch)
    expect(await screen.findByText('batch:1/3')).toBeInTheDocument();

    // Click Play
    await act(async () => {
      fireEvent.click(playBtn);
    });

    // Advance one tick (1 second at 1x speed)
    await act(async () => {
      jest.advanceTimersByTime(1100);
    });

    // cursor should have advanced to batch 2
    await waitFor(() => {
      expect(screen.getByText('batch:2/3')).toBeInTheDocument();
    });

    jest.useRealTimers();
  });

  test('playback tab is hidden for non-completed sessions', async () => {
    const pendingSession = makeSession({ sessionid: 'rpl-pending-1', status: 'pending' });
    mockListReplaySessions.mockResolvedValue({ items: [pendingSession], total: 1 });
    mockGetReplaySummary.mockResolvedValue({ sessionid: 'rpl-pending-1' });
    mockGetReplayAgentLogs.mockResolvedValue({ items: [], total: 0 });

    renderReplayPage();

    await waitFor(() => expect(mockListReplaySessions).toHaveBeenCalled());

    // Should NOT have a playback tab
    await waitFor(() => {
      expect(screen.queryByRole('tab', { name: 'replay.tabs.playback' })).not.toBeInTheDocument();
    });
  });

  test('playback shows noData message when agent logs are empty', async () => {
    const completedSession = makeSession({
      sessionid: 'rpl-empty-1',
      status: 'completed',
      cancancel: false,
      canretry: false,
    });

    mockListReplaySessions.mockResolvedValue({ items: [completedSession], total: 1 });
    mockGetReplaySummary.mockResolvedValue({ sessionid: 'rpl-empty-1' });
    mockGetReplayAgentLogs.mockResolvedValue({ items: [], total: 0 });

    renderReplayPage();

    await waitFor(() => expect(mockListReplaySessions).toHaveBeenCalled());

    const playbackTab = await screen.findByRole('tab', { name: 'replay.tabs.playback' });
    await act(async () => {
      fireEvent.click(playbackTab);
    });

    await waitFor(() => {
      expect(screen.getByText('replay.playback.noData')).toBeInTheDocument();
    });
  });

  test('playback scrubber seek updates the displayed frame', async () => {
    const completedSession = makeSession({
      sessionid: 'rpl-seek-1',
      status: 'completed',
      cancancel: false,
      canretry: false,
    });

    mockListReplaySessions.mockResolvedValue({ items: [completedSession], total: 1 });
    mockGetReplaySummary.mockResolvedValue({ sessionid: 'rpl-seek-1' });
    mockGetReplayAgentLogs.mockImplementation(async (_sessionid, opts) => {
      if (opts && opts.limit === 200) {
        return {
          items: [
            { batchseq: 1, pnl: 1, fillrate: 80, solvetime_ms: 10, residualnorm: 0.001, riskstatus: 'OK', solvererrorflag: false, batchresult: {} },
            { batchseq: 2, pnl: 2, fillrate: 85, solvetime_ms: 12, residualnorm: 0.001, riskstatus: 'OK', solvererrorflag: false, batchresult: {} },
            { batchseq: 3, pnl: 3, fillrate: 90, solvetime_ms: 15, residualnorm: 0.001, riskstatus: 'OK', solvererrorflag: false, batchresult: {} },
          ],
          total: 3,
        };
      }
      return { items: [], total: 0 };
    });

    renderReplayPage();

    await waitFor(() => expect(mockListReplaySessions).toHaveBeenCalled());

    const playbackTab = await screen.findByRole('tab', { name: 'replay.tabs.playback' });
    await act(async () => {
      fireEvent.click(playbackTab);
    });

    // Wait for buffer loaded (idle state, play button visible)
    const playBtn = await screen.findByRole('button', { name: 'replay.playback.play' });
    expect(playBtn).toBeInTheDocument();

    // Initially at batch 1
    expect(screen.getByText('batch:1/3')).toBeInTheDocument();

    // Seek to index 2 (batch 3)
    const scrubber = screen.getByLabelText('replay.playback.seek');
    await act(async () => {
      fireEvent.change(scrubber, { target: { value: '2' } });
    });

    // Should show batch 3
    await waitFor(() => {
      expect(screen.getByText('batch:3/3')).toBeInTheDocument();
    });
  });
});
