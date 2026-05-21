const mockGet = jest.fn();
const mockPost = jest.fn();

jest.mock('axios', () => ({
  create: jest.fn(() => ({
    get: mockGet,
    post: mockPost,
  })),
}));

describe('hedgeFlowService manual override helpers', () => {
  beforeEach(() => {
    jest.resetModules();
    mockGet.mockReset();
    mockPost.mockReset();
  });

  test('getManualOverrideContext normalizes source alerts and recent intents', async () => {
    mockGet.mockResolvedValue({
      data: {
        defaults: {
          side: 'SELL',
          urgency: 'MEDIUM',
          timeoutMs: 120000,
        },
        options: {
          symbols: ['BTC/USDT'],
          venues: ['coinbase'],
          providers: ['provider-alpha'],
          sides: ['BUY', 'SELL'],
          urgencies: ['LOW', 'MEDIUM', 'HIGH'],
        },
        source_alerts: [
          {
            alert_id: 'HEDGE_UNDERFILL:hgf-1',
            alert_type: 'HEDGE_UNDERFILL',
            hedge_flow_id: 'hgf-1',
            symbol: 'BTC/USDT',
            side: 'sell',
            gap_qty: '0.46',
            venue_ids: ['coinbase'],
          },
        ],
        recent: [
          {
            intent_id: 'manual-intent-1',
            hedge_flow_id: 'manual-hgf-1',
            source_alert_id: 'HEDGE_UNDERFILL:hgf-1',
            source_hedge_flow_id: 'hgf-1',
            provider_id: 'provider-alpha',
            symbol: 'BTC/USDT',
            side: 'sell',
            target_qty: '0.46',
            target_notional: '31328.42',
            venue_ids: ['coinbase'],
            urgency: 'medium',
            price_constraint: '68100',
            timeout_ms: '90000',
            reference_mid: '68105.25',
            max_slippage_bps: '15',
            risk_status: 'accepted',
            risk_decision: 'ALLOW',
            hedge_mode: 'operator_override',
            route_plan: [
              { venue_id: 'coinbase', order_type: 'IOC', split_qty: '0.46', sequence: 1 },
            ],
            source_topic: 'execution.intents',
            created_at: '2026-05-04T06:20:00.000Z',
          },
        ],
        summary: {
          source_alerts: '1',
          recent: '1',
          accepted: '1',
          rejected: '0',
        },
        generated_at: '2026-05-04T06:20:01.000Z',
        source: 'mock:execution.intents',
      },
    });

    const { getManualOverrideContext } = require('../hedgeFlowService');
    const response = await getManualOverrideContext({ sourceAlertId: '', status: 'all' });

    expect(mockGet).toHaveBeenCalledWith('/manual-overrides', { params: {} });
    expect(response.sourceAlerts[0]).toMatchObject({
      alertId: 'HEDGE_UNDERFILL:hgf-1',
      hedgeFlowId: 'hgf-1',
      side: 'SELL',
      gapQty: 0.46,
      venueIds: ['coinbase'],
    });
    expect(response.recent[0]).toMatchObject({
      intentId: 'manual-intent-1',
      hedgeFlowId: 'manual-hgf-1',
      sourceAlertId: 'HEDGE_UNDERFILL:hgf-1',
      side: 'SELL',
      targetQty: 0.46,
      targetNotional: 31328.42,
      venueIds: ['coinbase'],
      urgency: 'MEDIUM',
      priceConstraint: 68100,
      timeoutMs: 90000,
      referenceMid: 68105.25,
      maxSlippageBps: 15,
      riskStatus: 'ACCEPTED',
      routePlan: [{ venueId: 'coinbase', orderType: 'IOC', splitQty: 0.46, sequence: 1 }],
    });
    expect(response.summary).toMatchObject({
      sourceAlerts: 1,
      recent: 1,
      accepted: 1,
      rejected: 0,
    });
    expect(response.generatedAt).toBe('2026-05-04T06:20:01.000Z');
  });

  test('createManualOverride posts payload and normalizes response', async () => {
    mockPost.mockResolvedValue({
      data: {
        ok: true,
        intent: {
          intent_id: 'manual-intent-2',
          symbol: 'ETH/USDT',
          side: 'BUY',
          target_qty: '3.8',
          venue_ids: ['uniswap_v3'],
          urgency: 'HIGH',
          risk_status: 'ACCEPTED',
          route_plan: [
            { venue_id: 'uniswap_v3', order_type: 'MARKET', split_qty: '3.8', sequence: 1 },
          ],
        },
        context: {
          recent: [],
          source_alerts: [],
          summary: {},
        },
      },
    });

    const payload = {
      sourceAlertId: 'HEDGE_UNDERFILL:hgf-eth',
      symbol: 'ETH/USDT',
      side: 'BUY',
      targetQty: 3.8,
      venueIds: ['uniswap_v3'],
      urgency: 'HIGH',
      reason: 'Close residual exposure',
    };
    const { createManualOverride } = require('../hedgeFlowService');
    const response = await createManualOverride(payload);

    expect(mockPost).toHaveBeenCalledWith('/manual-overrides', payload);
    expect(response.intent).toMatchObject({
      intentId: 'manual-intent-2',
      symbol: 'ETH/USDT',
      side: 'BUY',
      targetQty: 3.8,
      venueIds: ['uniswap_v3'],
      urgency: 'HIGH',
      riskStatus: 'ACCEPTED',
      routePlan: [{ venueId: 'uniswap_v3', orderType: 'MARKET', splitQty: 3.8, sequence: 1 }],
    });
  });
});
