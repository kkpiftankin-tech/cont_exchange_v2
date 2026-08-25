const mockGet = jest.fn();
const mockPost = jest.fn();
const mockDelete = jest.fn();

jest.mock('axios', () => ({
  create: jest.fn(() => ({
    get: mockGet,
    post: mockPost,
    delete: mockDelete,
  })),
}));

describe('replayService session helpers', () => {
  beforeEach(() => {
    jest.resetModules();
    mockGet.mockReset();
    mockPost.mockReset();
    mockDelete.mockReset();
  });

  test('listReplaySessions normalizes progress from batch counters', async () => {
    mockGet.mockResolvedValue({
      data: {
        items: [
          {
            sessionid: 'rpl-live-1',
            status: 'running',
            progressbatches: 25,
            totalbatches: 100,
            createdat: '2026-04-26T10:00:00.000Z',
          },
        ],
      },
    });

    const { listReplaySessions } = require('../replayService');
    const response = await listReplaySessions();

    expect(response.items).toHaveLength(1);
    expect(response.items[0]).toMatchObject({
      sessionid: 'rpl-live-1',
      status: 'running',
      progressbatches: 25,
      totalbatches: 100,
      progress: 25,
      cancancel: true,
      canretry: false,
    });
  });

  test('cancelReplaySession propagates API errors instead of fabricating local state', async () => {
    mockDelete.mockRejectedValue(new Error('offline'));

    const { cancelReplaySession } = require('../replayService');
    await expect(cancelReplaySession('rpl-live-1')).rejects.toThrow('offline');
  });

  test('cancelReplaySession uses backend cancelled status response', async () => {
    mockDelete.mockResolvedValue({
      data: {
        accepted: true,
        sessionid: 'rpl-live-1',
        status: 'cancelled',
        partialsummary: true,
      },
    });

    const { cancelReplaySession } = require('../replayService');
    const session = await cancelReplaySession('rpl-live-1');
    expect(session).toMatchObject({
      sessionid: 'rpl-live-1',
      status: 'cancelled',
      cancancel: false,
      canretry: true,
      partialsummary: true,
    });
  });

  test('createReplaySession uses backend returned session id', async () => {
    mockPost.mockResolvedValue({
      data: {
        sessionid: 'backend-session-id',
        status: 'pending',
        createdat: '2026-04-26T10:00:00.000Z',
      },
    });

    const { createReplaySession } = require('../replayService');
    const created = await createReplaySession({ name: 'Replay' });
    expect(created.session.sessionid).toBe('backend-session-id');
    expect(mockPost).toHaveBeenCalledWith('/v1/replay/sessions', { name: 'Replay' });
  });

  test('getReplayAgentLogs normalizes F-15 aliases and JSON detail payloads', async () => {
    mockGet.mockResolvedValue({
      data: {
        items: [
          {
            logid: 'log-1',
            session_id: 'sess-1',
            batch_seq: 7,
            original_batch_id: 'batch-7',
            pnl: 12,
            is_value: 4.5,
            fill_rate: 50,
            solve_time_ms: 13,
            residual_norm: 0.01,
            risk_status: 'alert',
            solver_error_flag: true,
            error_code: 'residual_norm_above_tolerance',
            state: '{"cumPnL":12}',
            action: '[{"symbol":"BTCUSDT"}]',
            fills: '[{"execqty":2}]',
            batch_result: '{"riskstatus":"alert"}',
            metrics: '{"requestedqty":4}',
          },
        ],
        total: 1,
      },
    });

    const { getReplayAgentLogs } = require('../replayService');
    const response = await getReplayAgentLogs('sess-1', { is_value: 4.5 });

    expect(mockGet).toHaveBeenCalledWith('/v1/replay/sessions/sess-1/agentlogs', {
      params: { is_value: 4.5 },
    });
    expect(response.items[0]).toMatchObject({
      sessionid: 'sess-1',
      batchseq: 7,
      batch_seq: 7,
      originalbatchid: 'batch-7',
      original_batch_id: 'batch-7',
      isvalue: 4.5,
      is_value: 4.5,
      fillrate: 50,
      fill_rate: 50,
      solvetime_ms: 13,
      solve_time_ms: 13,
      residualnorm: 0.01,
      residual_norm: 0.01,
      riskstatus: 'alert',
      risk_status: 'alert',
      solvererrorflag: true,
      solver_error_flag: true,
      errorcode: 'residual_norm_above_tolerance',
      error_code: 'residual_norm_above_tolerance',
      state: { cumPnL: 12 },
      action: [{ symbol: 'BTCUSDT' }],
      fills: [{ execqty: 2 }],
      batchresult: { riskstatus: 'alert' },
      metrics: { requestedqty: 4 },
    });
  });

  test('normalizes snake_case session payloads and retry ids', async () => {
    mockPost.mockResolvedValue({
      data: {
        session_id: 'retry-session-id',
        retryparentid: 'original-session-id',
        status: 'pending',
        created_at: 1778060000000,
      },
    });

    const { retryReplaySession } = require('../replayService');
    const response = await retryReplaySession('original-session-id');
    expect(response.session.sessionid).toBe('retry-session-id');
    expect(response.session.status).toBe('pending');
  });

  test('subscribeReplayResults uses session_id query parameter', () => {
    const urls = [];
    const originalWebSocket = window.WebSocket;
    const originalEventSource = window.EventSource;
    window.WebSocket = jest.fn(() => {
      throw new Error('websocket unavailable');
    });
    window.EventSource = jest.fn(function EventSource(url) {
      urls.push(String(url));
      this.close = jest.fn();
    });

    const { subscribeReplayResults } = require('../replayService');
    const unsubscribe = subscribeReplayResults({ sessionid: 'rpl-live-1' });
    expect(urls).toHaveLength(1);
    expect(urls[0]).toContain('session_id=rpl-live-1');
    expect(urls[0]).not.toContain('sessionid=');
    unsubscribe();

    window.WebSocket = originalWebSocket;
    window.EventSource = originalEventSource;
  });

  test('subscribeReplayResults opens WebSocket endpoint before SSE fallback', () => {
    const urls = [];
    const originalWebSocket = window.WebSocket;
    const originalEventSource = window.EventSource;
    window.WebSocket = jest.fn(function WebSocket(url) {
      urls.push(String(url));
      this.close = jest.fn();
    });
    window.EventSource = jest.fn();

    const { subscribeReplayResults } = require('../replayService');
    const unsubscribe = subscribeReplayResults({ sessionid: 'rpl-live-1' });

    expect(urls).toHaveLength(1);
    expect(urls[0]).toContain('/v1/replay/results/ws');
    expect(urls[0]).toContain('session_id=rpl-live-1');
    expect(window.EventSource).not.toHaveBeenCalled();
    unsubscribe();

    window.WebSocket = originalWebSocket;
    window.EventSource = originalEventSource;
  });

  test('subscribeReplayResults derives final progress from lifecycle summary', () => {
    let sourceInstance;
    const events = [];
    const originalWebSocket = window.WebSocket;
    const originalEventSource = window.EventSource;
    window.WebSocket = jest.fn(() => {
      throw new Error('websocket unavailable');
    });
    window.EventSource = jest.fn(function EventSource() {
      sourceInstance = this;
      this.close = jest.fn();
    });

    const { subscribeReplayResults } = require('../replayService');
    const unsubscribe = subscribeReplayResults({
      sessionid: 'rpl-live-1',
      onEvent: (event) => events.push(event),
    });
    sourceInstance.onmessage({
      data: JSON.stringify({
        kind: 'lifecycle',
        session_id: 'rpl-live-1',
        status: 'completed',
        total_batches: 10,
        has_summary: true,
        summary: {
          processed_batches: 8,
          failed_batches: 2,
          total_fill_events: 4,
          partial: false,
        },
      }),
    });

    expect(events[0]).toMatchObject({
      sessionid: 'rpl-live-1',
      status: 'completed',
      progressbatches: 10,
      totalbatches: 10,
    });
    expect(events[0].summary).toMatchObject({
      processedbatches: 8,
      failedbatches: 2,
      totalbatches: 10,
    });
    unsubscribe();
    window.WebSocket = originalWebSocket;
    window.EventSource = originalEventSource;
  });

  test('getReplayAgentLogs parses JSON payload fields from API rows', async () => {
    mockGet.mockResolvedValue({
      data: {
        items: [
          {
            logid: 'log-1',
            batchseq: 1,
            state_json: '{"positions":[{"symbol":"BTCUSDT","qty":1}]}',
            action_json: '[{"symbol":"BTCUSDT","side":"buy"}]',
            fills_json: '[{"price":60100,"execqty":0.5}]',
            batch_result_json: '{"residualnorm":0.01}',
            metrics_json: '{"cumPnL":12}',
          },
        ],
      },
    });

    const { getReplayAgentLogs } = require('../replayService');
    const response = await getReplayAgentLogs('rpl-live-1');
    expect(response.items[0].state.positions[0].symbol).toBe('BTCUSDT');
    expect(response.items[0].action[0].side).toBe('buy');
    expect(response.items[0].fills[0].price).toBe(60100);
    expect(response.items[0].batchresult.residualnorm).toBe(0.01);
    expect(response.items[0].metrics.cumPnL).toBe(12);
  });
});
