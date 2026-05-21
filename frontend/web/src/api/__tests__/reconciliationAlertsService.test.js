const mockGet = jest.fn();

jest.mock('axios', () => ({
  create: jest.fn(() => ({
    get: mockGet,
  })),
}));

describe('hedgeFlowService reconciliation alert helpers', () => {
  beforeEach(() => {
    jest.resetModules();
    mockGet.mockReset();
  });

  test('getReconciliationAlerts compacts filters and normalizes aliases', async () => {
    mockGet.mockResolvedValue({
      data: {
        items: [
          {
            alert_id: 'HEDGE_UNDERFILL:hgf-1',
            alert_type: 'hedge_underfill',
            severity: 'WARNING',
            hedge_flow_id: 'hgf-1',
            intent_id: 'int-1',
            batch_id: 'batch-1',
            provider_id: 'provider-alpha',
            symbol: 'ETH/USDT',
            side: 'sell',
            status: 'underfilled',
            reconciliation_status: 'underfilled',
            venue_ids: ['coinbase', 'uniswap_v3'],
            target_qty: '18',
            filled_qty: '14.2',
            gap_qty: '3.8',
            gap_pct: '21.11',
            target_notional: '56880',
            avg_fill_price: '3154.4',
            reference_mid: '3160.1',
            hedge_pnl: '38.24',
            slippage_bps: '-18.04',
            total_fee: '42.7',
            fee_currency: 'USDT',
            hedge_mode: 'auto_after_batch',
            timeout_ms: '120000',
            timestamp: '2026-05-04T05:47:30.000Z',
            next_action: 'Raise HEDGE_UNDERFILLED.',
            status_reason: 'Depth vanished.',
            risk_decision: 'ALLOW',
            risk_limit_usage_pct: '79',
            source_topic: 'risk.alerts',
          },
        ],
        total: 1,
        summary: {
          total: '1',
          critical: 0,
          warning: 1,
          underfilled: 1,
          rejected: 0,
          total_gap_qty: '3.8',
          avg_gap_pct: '21.11',
          latest_alert_at: '2026-05-04T05:47:30.000Z',
        },
        filters: {
          symbols: ['ETH/USDT'],
          venues: ['coinbase'],
          providers: ['provider-alpha'],
          statuses: ['UNDERFILLED'],
          severities: ['warning'],
          types: ['HEDGE_UNDERFILL'],
        },
        generated_at: '2026-05-04T05:48:00.000Z',
        source: 'mock:risk.alerts',
      },
    });

    const { getReconciliationAlerts } = require('../hedgeFlowService');
    const response = await getReconciliationAlerts({
      status: 'UNDERFILLED',
      severity: 'warning',
      venue: 'all',
      providerId: '',
    });

    expect(mockGet).toHaveBeenCalledWith('/reconciliation-alerts', {
      params: { status: 'UNDERFILLED', severity: 'warning' },
    });
    expect(response.items[0]).toMatchObject({
      alertId: 'HEDGE_UNDERFILL:hgf-1',
      type: 'HEDGE_UNDERFILL',
      severity: 'warning',
      hedgeFlowId: 'hgf-1',
      batchId: 'batch-1',
      providerId: 'provider-alpha',
      side: 'SELL',
      status: 'UNDERFILLED',
      reconciliationStatus: 'UNDERFILLED',
      venueIds: ['coinbase', 'uniswap_v3'],
      targetQty: 18,
      filledQty: 14.2,
      gapQty: 3.8,
      gapPct: 21.11,
      avgFillPrice: 3154.4,
      referenceMid: 3160.1,
      hedgePnl: 38.24,
      slippageBps: -18.04,
      totalFee: 42.7,
      timeoutMs: 120000,
      riskLimitUsagePct: 79,
      sourceTopic: 'risk.alerts',
    });
    expect(response.summary).toMatchObject({
      total: 1,
      warning: 1,
      underfilled: 1,
      totalGapQty: 3.8,
      avgGapPct: 21.11,
      latestAlertAt: '2026-05-04T05:47:30.000Z',
    });
    expect(response.filters.types).toEqual(['HEDGE_UNDERFILL']);
    expect(response.generatedAt).toBe('2026-05-04T05:48:00.000Z');
  });

  test('normalizeReconciliationAlertsResponse tolerates empty payloads', () => {
    const { normalizeReconciliationAlertsResponse } = require('../hedgeFlowService');
    const response = normalizeReconciliationAlertsResponse();

    expect(response.items).toEqual([]);
    expect(response.total).toBe(0);
    expect(response.filters).toEqual({
      symbols: [],
      venues: [],
      providers: [],
      statuses: [],
      severities: [],
      types: [],
    });
    expect(response.summary).toMatchObject({
      total: 0,
      critical: 0,
      warning: 0,
      underfilled: 0,
      rejected: 0,
    });
  });
});
