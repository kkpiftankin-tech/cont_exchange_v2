const mockGet = jest.fn();
const mockPut = jest.fn();

jest.mock('axios', () => ({
  create: jest.fn(() => ({
    get: mockGet,
    put: mockPut,
  })),
}));

describe('hedgeFlowService policy config helpers', () => {
  beforeEach(() => {
    jest.resetModules();
    mockGet.mockReset();
    mockPut.mockReset();
  });

  test('getPolicyConfig normalizes solverconfig aliases', async () => {
    mockGet.mockResolvedValue({
      data: {
        config: {
          solver_config_id: 'solver-prod-v4',
          revision: '3',
          hedge_trigger_threshold: {
            'BTC/USDT': '0.25',
            'ETH/USDT': '5',
          },
          hedge_urgency_policy: {
            low: { min_gap_pct: '0', order_type: 'limit', timeout_ms: '180000' },
            MEDIUM: { minGapPct: '10', orderType: 'ioc', timeoutMs: '120000' },
            HIGH: { min_gap_pct: '25', order_type: 'market', timeout_ms: '60000' },
          },
          max_slippage_bps: {
            LOW: '8',
            MEDIUM: '15',
            HIGH: '25',
          },
          risk_limits: {
            hedge_exposure_limit: '250000',
            max_notional_per_hedge: '100000',
          },
          updated_by: 'operator',
          updated_at: '2026-05-04T06:00:00.000Z',
        },
        options: {
          urgencies: ['LOW', 'MEDIUM', 'HIGH'],
          order_types: ['LIMIT', 'IOC', 'MARKET'],
          symbols: ['BTC/USDT', 'ETH/USDT'],
        },
        impact: [
          {
            hedge_flow_id: 'hgf-1',
            symbol: 'ETH/USDT',
            status: 'underfilled',
            gap_qty: '3.8',
            threshold_qty: '5',
            gap_pct: '21.11',
            eligible: false,
            urgency: 'high',
            max_slippage_bps: '25',
          },
        ],
        audit: [
          {
            revision: '3',
            actor: 'operator',
            reason: 'Tighten high urgency',
            updated_at: '2026-05-04T06:00:00.000Z',
            changed_fields: ['maxSlippageBps'],
          },
        ],
        summary: {
          revision: '3',
          symbols: '2',
          eligible_flows: '1',
          avg_max_slippage_bps: '16',
          max_threshold_qty: '5',
          hedge_exposure_limit: '250000',
          max_notional_per_hedge: '100000',
        },
        generated_at: '2026-05-04T06:01:00.000Z',
        source: 'mock:solverconfig',
      },
    });

    const { getPolicyConfig } = require('../hedgeFlowService');
    const response = await getPolicyConfig();

    expect(mockGet).toHaveBeenCalledWith('/policy-config');
    expect(response.config).toMatchObject({
      solverConfigId: 'solver-prod-v4',
      revision: 3,
      hedgeTriggerThreshold: {
        'BTC/USDT': 0.25,
        'ETH/USDT': 5,
      },
      maxSlippageBps: {
        LOW: 8,
        MEDIUM: 15,
        HIGH: 25,
      },
      riskLimits: {
        hedgeExposureLimit: 250000,
        maxNotionalPerHedge: 100000,
      },
      updatedBy: 'operator',
      updatedAt: '2026-05-04T06:00:00.000Z',
    });
    expect(response.config.hedgeUrgencyPolicy.LOW).toMatchObject({
      minGapPct: 0,
      orderType: 'LIMIT',
      timeoutMs: 180000,
    });
    expect(response.impact[0]).toMatchObject({
      hedgeFlowId: 'hgf-1',
      status: 'UNDERFILLED',
      gapQty: 3.8,
      thresholdQty: 5,
      urgency: 'HIGH',
      maxSlippageBps: 25,
    });
    expect(response.audit[0]).toMatchObject({
      revision: 3,
      changedFields: ['maxSlippageBps'],
    });
    expect(response.summary).toMatchObject({
      revision: 3,
      eligibleFlows: 1,
      avgMaxSlippageBps: 16,
      hedgeExposureLimit: 250000,
    });
  });

  test('updatePolicyConfig sends payload and normalizes updated response', async () => {
    const payload = {
      reason: 'Reduce HIGH slippage',
      config: {
        solverConfigId: 'solver-prod-v4',
        maxSlippageBps: { LOW: 8, MEDIUM: 15, HIGH: 5 },
      },
    };
    mockPut.mockResolvedValue({
      data: {
        config: {
          solverConfigId: 'solver-prod-v4',
          revision: 4,
          maxSlippageBps: { LOW: 8, MEDIUM: 15, HIGH: 5 },
        },
        summary: { revision: 4 },
      },
    });

    const { updatePolicyConfig } = require('../hedgeFlowService');
    const response = await updatePolicyConfig(payload);

    expect(mockPut).toHaveBeenCalledWith('/policy-config', payload);
    expect(response.config.maxSlippageBps.HIGH).toBe(5);
    expect(response.summary.revision).toBe(4);
  });
});
