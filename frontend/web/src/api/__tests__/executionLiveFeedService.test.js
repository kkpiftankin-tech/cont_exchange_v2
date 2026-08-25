const mockGet = jest.fn();
let originalWebSocket;

jest.mock('axios', () => ({
  create: jest.fn(() => ({
    get: mockGet,
  })),
}));

describe('executionLiveFeedService', () => {
  beforeEach(() => {
    jest.resetModules();
    mockGet.mockReset();
    originalWebSocket = window.WebSocket;
  });

  afterEach(() => {
    window.WebSocket = originalWebSocket;
  });

  test('getExecutionLiveSnapshot compacts filters and normalizes F-12 report aliases', async () => {
    mockGet.mockResolvedValue({
      data: {
        items: [
          {
            execution_id: 'exec-1',
            hedge_flow_id: 'hgf-1',
            child_order_id: 'chd-1',
            venue_id: 'binance',
            avgPrice: '68123.45',
            filled_qty: '0.42',
            slippage_bps: '-2.5',
            hedge_pnl: '12.3',
            status: 'filled',
          },
        ],
        filters: {
          symbols: ['BTC/USDT'],
          venues: ['binance'],
          providers: ['provider-alpha'],
          statuses: ['FILLED'],
          sides: ['SELL'],
        },
      },
    });

    const { getExecutionLiveSnapshot } = require('../executionLiveFeedService');
    const response = await getExecutionLiveSnapshot({
      symbol: 'all',
      venue: 'binance',
      status: '',
      search: 'all',
      limit: 50,
    });

    expect(mockGet).toHaveBeenCalledWith('/executions/live', {
      params: {
        venue: 'binance',
        search: 'all',
        limit: 50,
      },
    });
    expect(response.items[0]).toMatchObject({
      executionId: 'exec-1',
      hedgeFlowId: 'hgf-1',
      childOrderId: 'chd-1',
      venueId: 'binance',
      avgFillPrice: 68123.45,
      filledQty: 0.42,
      slippageBps: -2.5,
      hedgePnl: 12.3,
      status: 'FILLED',
    });
    expect(response.items[0].notional).toBeCloseTo(28611.849);
  });

  test('buildExecutionLiveFeedWsUrl derives a browser WebSocket URL from the API base', () => {
    const { buildExecutionLiveFeedWsUrl } = require('../executionLiveFeedService');
    const url = new URL(buildExecutionLiveFeedWsUrl({
      symbol: 'BTC/USDT',
      venue: 'all',
      search: 'exec 1',
    }));

    expect(url.protocol).toBe('ws:');
    expect(url.host).toBe(window.location.host);
    expect(url.pathname).toBe('/api/executions/live/ws');
    expect(url.searchParams.get('symbol')).toBe('BTC/USDT');
    expect(url.searchParams.get('search')).toBe('exec 1');
    expect(url.searchParams.has('venue')).toBe(false);
  });

  test('parseExecutionLiveFeedMessage normalizes snapshot, report, and heartbeat payloads', () => {
    const { parseExecutionLiveFeedMessage } = require('../executionLiveFeedService');

    const snapshot = parseExecutionLiveFeedMessage(JSON.stringify({
      type: 'snapshot',
      payload: {
        reports: [{ execution_id: 'exec-snapshot', avg_price: '101' }],
      },
    }));
    const report = parseExecutionLiveFeedMessage(JSON.stringify({
      type: 'execution_report',
      item: { execId: 'exec-live', avgPrice: '102', live: true },
      generated_at: '2026-05-04T06:15:00.000Z',
    }));
    const heartbeat = parseExecutionLiveFeedMessage(JSON.stringify({
      type: 'heartbeat',
      generated_at: '2026-05-04T06:15:01.000Z',
    }));

    expect(snapshot.payload.items[0].executionId).toBe('exec-snapshot');
    expect(report.item).toMatchObject({
      executionId: 'exec-live',
      avgFillPrice: 102,
      live: true,
    });
    expect(report.generatedAt).toBe('2026-05-04T06:15:00.000Z');
    expect(heartbeat.generatedAt).toBe('2026-05-04T06:15:01.000Z');
  });

  test('subscribeExecutionLiveFeed opens WebSocket and dispatches normalized messages', () => {
    const sockets = [];

    class FakeWebSocket {
      constructor(url) {
        this.url = url;
        this.close = jest.fn(() => {
          this.onclose?.();
        });
        sockets.push(this);
      }
    }

    window.WebSocket = FakeWebSocket;

    const { subscribeExecutionLiveFeed } = require('../executionLiveFeedService');
    const snapshots = [];
    const reports = [];
    const heartbeats = [];
    const states = [];
    const unsubscribe = subscribeExecutionLiveFeed({
      params: { venue: 'binance' },
      onSnapshot: (payload) => snapshots.push(payload),
      onReport: (item) => reports.push(item),
      onHeartbeat: (message) => heartbeats.push(message),
      onState: (state) => states.push(state),
      reconnectMs: 10,
    });

    expect(sockets).toHaveLength(1);
    expect(sockets[0].url).toContain('/api/executions/live/ws?venue=binance');

    sockets[0].onopen();
    sockets[0].onmessage({
      data: JSON.stringify({
        type: 'snapshot',
        payload: { items: [{ execution_id: 'exec-1', avgPrice: '10' }] },
      }),
    });
    sockets[0].onmessage({
      data: JSON.stringify({
        type: 'execution_report',
        item: { execution_id: 'exec-2', filled_qty: '2' },
      }),
    });
    sockets[0].onmessage({
      data: JSON.stringify({
        type: 'heartbeat',
        generatedAt: '2026-05-04T06:15:02.000Z',
      }),
    });

    expect(states).toEqual(['connecting', 'connected']);
    expect(snapshots[0].items[0].avgFillPrice).toBe(10);
    expect(reports[0]).toMatchObject({ executionId: 'exec-2', filledQty: 2 });
    expect(heartbeats[0].generatedAt).toBe('2026-05-04T06:15:02.000Z');

    unsubscribe();
    expect(sockets[0].close).toHaveBeenCalled();

  });
});
