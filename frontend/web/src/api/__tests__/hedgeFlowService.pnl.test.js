const mockGet = jest.fn();

jest.mock('axios', () => ({
  create: jest.fn(() => ({
    get: mockGet,
  })),
}));

describe('hedgeFlowService Hedge PnL dashboard helpers', () => {
  beforeEach(() => {
    jest.resetModules();
    mockGet.mockReset();
  });

  test('getHedgePnlDashboard compacts filters and normalizes analytics aliases', async () => {
    mockGet.mockResolvedValue({
      data: {
        summary: {
          net_after_fees: '-59.5',
          total_fees: '31.8',
          total_notional: '50415.03',
          total_filled_qty: '0.74',
          report_count: 2,
          flow_count: 1,
          avg_slippage_bps: '-4.52',
          avg_price_spread: '-30.8',
          win_rate_pct: 50,
        },
        time_series: [
          {
            execution_id: 'exec-1',
            hedge_flow_id: 'hgf-1',
            batch_id: 'batch-1',
            provider_id: 'provider-alpha',
            venue_id: 'binance',
            symbol: 'BTC/USDT',
            side: 'sell',
            timestamp: '2026-05-04T06:15:17.000Z',
            filled_qty: '0.72',
            avg_price: '68131.5',
            reference_mid: '68105.25',
            fee: '29.44',
            slippage_bps: '3.85',
            hedge_pnl: '-48.34',
          },
          {
            execution_id: 'exec-2',
            venue_id: 'coinbase',
            symbol: 'BTC/USDT',
            timestamp: '2026-05-04T06:15:58.000Z',
            filled_qty: '0.02',
            avg_fill_price: '68017.4',
            clearing_price: '68105.25',
            fee_currency: 'USDT',
            hedge_pnl: '-10.67',
          },
        ],
        symbol_breakdown: [
          { symbol: 'BTC/USDT', hedge_pnl: '-59.01', report_count: 2, avg_slippage_bps: '-4.52' },
        ],
        venue_breakdown: [
          { venue_id: 'binance', hedge_pnl: '-48.34', fees: '29.44' },
        ],
        filters: {
          symbols: ['BTC/USDT'],
          venues: ['binance', 'coinbase'],
          providers: ['provider-alpha'],
        },
        generated_at: '2026-05-04T06:16:00.000Z',
      },
    });

    const { getHedgePnlDashboard } = require('../hedgeFlowService');
    const response = await getHedgePnlDashboard({
      symbol: 'all',
      venue: 'binance',
      providerId: '',
    });

    expect(mockGet).toHaveBeenCalledWith('/hedge-pnl', {
      params: { venue: 'binance' },
    });
    expect(response.summary).toMatchObject({
      netAfterFees: -59.5,
      grossBeforeFees: -27.7,
      totalFees: 31.8,
      totalNotional: 50415.03,
      totalFilledQty: 0.74,
      reportCount: 2,
      flowCount: 1,
      avgSlippageBps: -4.52,
      avgPriceSpread: -30.8,
      winRatePct: 50,
    });
    expect(response.timeSeries[0]).toMatchObject({
      executionId: 'exec-1',
      hedgeFlowId: 'hgf-1',
      batchId: 'batch-1',
      providerId: 'provider-alpha',
      venueId: 'binance',
      side: 'SELL',
      filledQty: 0.72,
      avgFillPrice: 68131.5,
      clearingPrice: 68105.25,
      referenceMid: 68105.25,
      fee: 29.44,
      feeCurrency: 'USDT',
      slippageBps: 3.85,
      hedgePnl: -48.34,
      cumulativePnl: -48.34,
    });
    expect(response.timeSeries[1]).toMatchObject({
      avgFillPrice: 68017.4,
      clearingPrice: 68105.25,
      cumulativePnl: -59.01,
    });
    expect(response.symbolBreakdown[0]).toMatchObject({
      id: 'BTC/USDT',
      hedgePnl: -59.01,
      reports: 2,
      avgSlippageBps: -4.52,
    });
    expect(response.venueBreakdown[0]).toMatchObject({
      id: 'binance',
      hedgePnl: -48.34,
      fees: 29.44,
    });
    expect(response.generatedAt).toBe('2026-05-04T06:16:00.000Z');
  });

  test('normalizeHedgePnlDashboardResponse tolerates empty payloads', () => {
    const { normalizeHedgePnlDashboardResponse } = require('../hedgeFlowService');
    const response = normalizeHedgePnlDashboardResponse();

    expect(response.timeSeries).toEqual([]);
    expect(response.symbolBreakdown).toEqual([]);
    expect(response.venueBreakdown).toEqual([]);
    expect(response.filters).toEqual({ symbols: [], venues: [], providers: [] });
    expect(response.summary).toMatchObject({
      netAfterFees: 0,
      grossBeforeFees: 0,
      totalFees: 0,
      reportCount: 0,
      flowCount: 0,
    });
  });
});
