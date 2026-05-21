import React from 'react';
import { act, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';

const mockGetHedgePnlDashboard = jest.fn();
const mockGetReconciliationAlerts = jest.fn();
const mockGetManualOverrideContext = jest.fn();
const mockCreateManualOverride = jest.fn();
const mockGetPolicyConfig = jest.fn();
const mockUpdatePolicyConfig = jest.fn();
const mockGetExecutionLiveSnapshot = jest.fn();
const mockSubscribeExecutionLiveFeed = jest.fn();
const mockAxiosGet = jest.fn();
const mockAxiosPost = jest.fn();
const mockAxiosPut = jest.fn();
const mockNavigate = jest.fn();
const mockLogout = jest.fn();
const mockIsAuthenticated = jest.fn();

let lastFeedCallbacks = null;
let mockSearchParams = '';

const translations = {
  'auth.loading': 'Loading...',
  'navbar.logo': 'CONT',
  'navbar.trade': 'trade',
  'navbar.profile': 'profile',
  'navbar.venues': 'venues',
  'navbar.hedgeflows': 'hedge',
  'navbar.hedgePnl': 'pnl',
  'navbar.executionLive': 'live',
  'navbar.reconciliationAlerts': 'alerts',
  'navbar.manualOverride': 'override',
  'navbar.policyConfig': 'policy',
  'navbar.replay': 'replay',
  'navbar.logout': 'Logout',
  'hedgePnl.title': 'Hedge PnL Dashboard',
  'hedgePnl.kicker': 'F-12 Analytics',
  'hedgePnl.subtitle': 'Hedge PnL by symbol and venue.',
  'hedgePnl.actions.refresh': 'Refresh',
  'hedgePnl.filters.symbol': 'Symbol',
  'hedgePnl.filters.venue': 'Venue',
  'hedgePnl.filters.provider': 'Provider',
  'hedgePnl.filters.all': 'All',
  'hedgePnl.table.title': 'Execution rows',
  'executionLive.title': 'Execution Live Feed',
  'executionLive.kicker': 'F-12 WebSocket',
  'executionLive.subtitle': 'Live execution reports.',
  'executionLive.actions.refresh': 'Refresh',
  'executionLive.actions.pause': 'Pause',
  'executionLive.actions.resume': 'Resume',
  'executionLive.connection.connected': 'connected',
  'executionLive.connection.paused': 'paused',
  'executionLive.filters.symbol': 'Symbol',
  'executionLive.filters.venue': 'Venue',
  'executionLive.filters.provider': 'Provider',
  'executionLive.filters.status': 'Status',
  'executionLive.filters.side': 'Side',
  'executionLive.filters.search': 'Search',
  'executionLive.filters.searchPlaceholder': 'Search execution',
  'executionLive.filters.all': 'All',
  'executionLive.feed.title': 'Live reports',
  'executionLive.feed.topic': 'execution.venue',
  'executionLive.feed.live': 'live',
  'executionLive.inspector.kicker': 'Execution',
  'reconciliationAlerts.title': 'Reconciliation Alerts',
  'reconciliationAlerts.kicker': 'F-12 Alerts',
  'reconciliationAlerts.subtitle': 'Residual hedge alerts.',
  'reconciliationAlerts.actions.refresh': 'Refresh',
  'reconciliationAlerts.actions.openManualOverride': 'Open manual override',
  'reconciliationAlerts.actions.openHedgeflows': 'Open hedgeflows',
  'reconciliationAlerts.filters.severity': 'Severity',
  'reconciliationAlerts.filters.status': 'Status',
  'reconciliationAlerts.filters.type': 'Type',
  'reconciliationAlerts.filters.symbol': 'Symbol',
  'reconciliationAlerts.filters.venue': 'Venue',
  'reconciliationAlerts.filters.provider': 'Provider',
  'reconciliationAlerts.filters.search': 'Search',
  'reconciliationAlerts.filters.searchPlaceholder': 'Search alert',
  'reconciliationAlerts.filters.all': 'All',
  'reconciliationAlerts.list.title': 'Alert list',
  'reconciliationAlerts.list.topic': 'risk.alerts',
  'reconciliationAlerts.inspector.kicker': 'Reconciliation',
  'manualOverride.title': 'Manual Override',
  'manualOverride.kicker': 'F-12 Operator UI',
  'manualOverride.subtitle': 'Create manual ExecutionIntent.',
  'manualOverride.actions.refresh': 'Refresh context',
  'manualOverride.form.title': 'ExecutionIntent',
  'manualOverride.form.subtitle': 'Manual operator hedge request.',
  'manualOverride.form.sourceAlert': 'Source alert',
  'manualOverride.form.noSourceAlert': 'No source alert',
  'manualOverride.form.symbol': 'Symbol',
  'manualOverride.form.side': 'Side',
  'manualOverride.form.targetQty': 'Target qty',
  'manualOverride.form.provider': 'Provider',
  'manualOverride.form.priceConstraint': 'Price constraint',
  'manualOverride.form.timeoutMs': 'Timeout ms',
  'manualOverride.form.urgency': 'Urgency',
  'manualOverride.form.venues': 'Venues',
  'manualOverride.form.reason': 'Operator reason',
  'manualOverride.form.submit': 'Create ExecutionIntent',
  'manualOverride.form.submitting': 'Creating...',
  'manualOverride.side.title': 'Override trail',
  'manualOverride.side.subtitle': 'Recent operator actions.',
  'manualOverride.side.source': 'Source alert',
  'manualOverride.side.created': 'Created intent',
  'manualOverride.side.recent': 'Recent intents',
  'manualOverride.states.noRecent': 'No manual override intents yet.',
  'policyConfig.title': 'Policy Config',
  'policyConfig.kicker': 'F-12 Admin UI',
  'policyConfig.subtitle': 'Edit solverconfig policy.',
  'policyConfig.actions.refresh': 'Refresh config',
  'policyConfig.actions.reset': 'Reset draft',
  'policyConfig.actions.save': 'Save policy',
  'policyConfig.actions.saving': 'Saving...',
  'policyConfig.form.title': 'solverconfig',
  'policyConfig.form.subtitle': 'Runtime policy values.',
  'policyConfig.form.general': 'General limits',
  'policyConfig.form.generalMeta': 'Risk envelope.',
  'policyConfig.form.solverConfigId': 'Solver config id',
  'policyConfig.form.hedgeExposureLimit': 'Hedge exposure limit',
  'policyConfig.form.maxNotionalPerHedge': 'Max notional per hedge',
  'policyConfig.form.thresholds': 'Trigger thresholds',
  'policyConfig.form.thresholdsMeta': 'Net residual qty trigger.',
  'policyConfig.form.urgency': 'Urgency policy',
  'policyConfig.form.urgencyMeta': 'Routing and slippage tiers.',
  'policyConfig.form.reason': 'Audit reason',
  'policyConfig.side.title': 'Impact preview',
  'policyConfig.side.subtitle': 'Current flows against policy.',
  'policyConfig.side.impact': 'Flow eligibility',
  'policyConfig.side.skip': 'SKIP',
  'policyConfig.side.audit': 'Audit trail',
  'policyConfig.side.risk': 'Risk view',
  'policyConfig.side.maxThreshold': 'Max threshold qty',
  'policyConfig.side.avgSlippage': 'Avg slippage cap',
  'policyConfig.side.coverage': 'Eligible coverage',
};

const translate = (key, options = {}) => {
  if (options.defaultValue) return options.defaultValue;
  if (key === 'hedgePnl.live' || key === 'reconciliationAlerts.live') return `live ${options.seconds}s`;
  if (key === 'hedgePnl.lastUpdated' || key === 'executionLive.lastEvent' || key === 'reconciliationAlerts.lastUpdated' || key === 'manualOverride.lastUpdated' || key === 'policyConfig.lastUpdated') return `updated ${options.time}`;
  if (key === 'executionLive.feed.subtitle') return `${options.count} reports`;
  if (key === 'reconciliationAlerts.list.subtitle') return `${options.count} alerts`;
  if (key === 'manualOverride.states.created') return `Created ${options.intentId}`;
  if (key === 'policyConfig.states.saved') return `Saved revision ${options.revision}`;
  if (key === 'policyConfig.side.threshold') return `threshold ${options.value}`;
  if (key === 'policyConfig.side.revision') return `rev ${options.revision}`;
  if (key === 'manualOverride.preview.urgency') return `${options.urgency} urgency policy`;
  return translations[key] || key;
};

jest.mock('react-i18next', () => ({
  useTranslation: () => ({ t: translate }),
}));

jest.mock('axios', () => ({
  create: jest.fn(() => ({
    get: mockAxiosGet,
    post: mockAxiosPost,
    put: mockAxiosPut,
  })),
}));

jest.mock('react-router-dom', () => ({
  useNavigate: () => mockNavigate,
  useSearchParams: () => [new URLSearchParams(mockSearchParams), jest.fn()],
}), { virtual: true });

jest.mock('../../../api/authService', () => ({
  isAuthenticated: (...args) => mockIsAuthenticated(...args),
  logout: () => mockLogout(),
}));

jest.mock('../../../api/hedgeFlowService', () => ({
  getHedgePnlDashboard: (...args) => mockGetHedgePnlDashboard(...args),
  getReconciliationAlerts: (...args) => mockGetReconciliationAlerts(...args),
  getManualOverrideContext: (...args) => mockGetManualOverrideContext(...args),
  createManualOverride: (...args) => mockCreateManualOverride(...args),
  getPolicyConfig: (...args) => mockGetPolicyConfig(...args),
  updatePolicyConfig: (...args) => mockUpdatePolicyConfig(...args),
}));

jest.mock('../../../api/executionLiveFeedService', () => ({
  getExecutionLiveSnapshot: (...args) => mockGetExecutionLiveSnapshot(...args),
  subscribeExecutionLiveFeed: (...args) => mockSubscribeExecutionLiveFeed(...args),
}));

jest.mock('recharts', () => {
  const React = require('react');
  const Shell = () => <div data-testid="chart" />;
  return {
    Area: () => <div />,
    Bar: () => <div />,
    BarChart: Shell,
    CartesianGrid: () => <div />,
    ComposedChart: Shell,
    Legend: () => <div />,
    Line: () => <div />,
    LineChart: Shell,
    ResponsiveContainer: Shell,
    Tooltip: () => <div />,
    XAxis: () => <div />,
    YAxis: () => <div />,
  };
});

const HedgePnlDashboard = require('../../HedgePnlDashboard/HedgePnlDashboard').default;
const ExecutionLiveFeed = require('../../ExecutionLiveFeed/ExecutionLiveFeed').default;
const ReconciliationAlerts = require('../../ReconciliationAlerts/ReconciliationAlerts').default;
const ManualOverride = require('../../ManualOverride/ManualOverride').default;
const PolicyConfig = require('../../PolicyConfig/PolicyConfig').default;

const executionSnapshot = {
  items: [
    {
      sequence: 1,
      executionId: 'exec-btc-001',
      hedgeFlowId: 'hgf-btc-001',
      childOrderId: 'chd-btc-001',
      clientOrderId: 'hgf-btc-001-01',
      batchId: 'batch-1',
      providerId: 'provider-alpha',
      symbol: 'BTC/USDT',
      venueId: 'binance',
      side: 'SELL',
      status: 'FILLED',
      urgency: 'HIGH',
      routeType: 'IOC',
      timestamp: '2026-05-04T06:15:17.000Z',
      receivedAt: '2026-05-04T06:15:17.036Z',
      filledQty: 0.72,
      avgFillPrice: 68131.5,
      referenceMid: 68105.25,
      notional: 49054.68,
      fee: 29.44,
      feeCurrency: 'USDT',
      slippageBps: 3.85,
      hedgePnl: -48.34,
      latencyMs: 36,
      sourceTopic: 'execution.venue',
    },
  ],
  total: 1,
  summary: {
    reports: 1,
    totalNotional: 49054.68,
    avgSlippageBps: 3.85,
    p95LatencyMs: 36,
    rejected: 0,
    slippageAlerts: 0,
  },
  filters: {
    symbols: ['BTC/USDT'],
    venues: ['binance'],
    providers: ['provider-alpha'],
    statuses: ['FILLED'],
    sides: ['SELL'],
  },
  generatedAt: '2026-05-04T06:16:00.000Z',
  source: 'mock:execution.venue',
};

const reconciliationResponse = {
  items: [
    {
      alertId: 'HEDGE_UNDERFILL:hgf-eth-003',
      type: 'HEDGE_UNDERFILL',
      severity: 'critical',
      hedgeFlowId: 'hgf-eth-003',
      batchId: 'batch-3',
      providerId: 'provider-alpha',
      symbol: 'ETH/USDT',
      side: 'SELL',
      status: 'UNDERFILLED',
      reconciliationStatus: 'UNDERFILLED',
      venueIds: ['coinbase'],
      targetQty: 12,
      filledQty: 8.2,
      gapQty: 3.8,
      gapPct: 31.6,
      referenceMid: 3160.1,
      slippageBps: 18.2,
      hedgePnl: -41.2,
      feeCurrency: 'USDT',
      urgency: 'HIGH',
      timeoutMs: 120000,
      timestamp: '2026-05-04T06:20:00.000Z',
      nextAction: 'Open manual override.',
      riskDecision: 'ALLOW',
      riskLimitUsagePct: 44,
      hedgeMode: 'auto',
      sourceTopic: 'risk.alerts',
    },
  ],
  total: 1,
  summary: {
    total: 1,
    critical: 1,
    underfilled: 1,
    rejected: 0,
    totalGapQty: 3.8,
    avgGapPct: 31.6,
  },
  filters: {
    symbols: ['ETH/USDT'],
    venues: ['coinbase'],
    providers: ['provider-alpha'],
    statuses: ['UNDERFILLED'],
    severities: ['critical'],
    types: ['HEDGE_UNDERFILL'],
  },
  generatedAt: '2026-05-04T06:21:00.000Z',
  source: 'mock:risk.alerts',
};

const manualContext = {
  defaults: {
    side: 'SELL',
    urgency: 'MEDIUM',
    timeoutMs: 120000,
    providerId: 'operator-manual',
  },
  options: {
    symbols: ['ETH/USDT'],
    venues: ['coinbase', 'binance'],
    providers: ['provider-alpha'],
    sides: ['BUY', 'SELL'],
    urgencies: ['LOW', 'MEDIUM', 'HIGH'],
  },
  sourceAlerts: reconciliationResponse.items,
  recent: [],
  summary: {
    sourceAlerts: 1,
    recent: 0,
    accepted: 0,
    rejected: 0,
  },
  generatedAt: '2026-05-04T06:21:00.000Z',
  source: 'mock:execution.intents',
};

const policyContext = {
  config: {
    solverConfigId: 'solver-prod-v4',
    revision: 1,
    hedgeTriggerThreshold: {
      'BTC/USDT': 0.25,
      'ETH/USDT': 5,
    },
    hedgeUrgencyPolicy: {
      LOW: { minGapPct: 0, orderType: 'POST_ONLY', timeoutMs: 180000 },
      MEDIUM: { minGapPct: 10, orderType: 'IOC', timeoutMs: 120000 },
      HIGH: { minGapPct: 25, orderType: 'MARKET', timeoutMs: 60000 },
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
    updatedBy: 'system',
    updatedAt: '2026-05-04T06:00:00.000Z',
  },
  options: {
    urgencies: ['LOW', 'MEDIUM', 'HIGH'],
    orderTypes: ['POST_ONLY', 'LIMIT', 'IOC', 'MARKET'],
    symbols: ['BTC/USDT', 'ETH/USDT'],
  },
  impact: [
    {
      hedgeFlowId: 'hgf-eth-003',
      symbol: 'ETH/USDT',
      status: 'UNDERFILLED',
      gapQty: 3.8,
      thresholdQty: 5,
      gapPct: 31.6,
      eligible: false,
      urgency: 'HIGH',
      maxSlippageBps: 25,
    },
  ],
  audit: [
    {
      revision: 1,
      actor: 'system',
      reason: 'Initial F-12 mock solverconfig.',
      updatedAt: '2026-05-04T06:00:00.000Z',
      changedFields: ['initial'],
    },
  ],
  summary: {
    revision: 1,
    symbols: 2,
    eligibleFlows: 0,
    avgMaxSlippageBps: 16,
    maxThresholdQty: 5,
    hedgeExposureLimit: 250000,
    maxNotionalPerHedge: 100000,
  },
  generatedAt: '2026-05-04T06:21:00.000Z',
  source: 'mock:solverconfig',
};

const pnlDashboard = {
  summary: {
    netAfterFees: -59.01,
    grossBeforeFees: -27.21,
    totalFees: 31.8,
    avgSlippageBps: 4.52,
    winRatePct: 0,
    reportCount: 2,
    totalNotional: 50415.03,
    totalFilledQty: 0.74,
    flowCount: 1,
    avgPriceSpread: -30.8,
  },
  timeSeries: [
    {
      executionId: 'exec-btc-001',
      hedgeFlowId: 'hgf-btc-001',
      batchId: 'batch-1',
      providerId: 'provider-alpha',
      symbol: 'BTC/USDT',
      venueId: 'binance',
      side: 'SELL',
      status: 'FILLED',
      urgency: 'HIGH',
      timestamp: '2026-05-04T06:15:17.000Z',
      filledQty: 0.72,
      feeCurrency: 'USDT',
      clearingPrice: 68105.25,
      referenceMid: 68105.25,
      avgFillPrice: 68131.5,
      priceSpread: 26.25,
      slippageBps: 3.85,
      hedgePnl: -48.34,
      cumulativePnl: -48.34,
    },
  ],
  symbolBreakdown: [{ id: 'BTC/USDT', hedgePnl: -59.01, reports: 2 }],
  venueBreakdown: [{ id: 'binance', hedgePnl: -48.34, reports: 1 }],
  filters: {
    symbols: ['BTC/USDT'],
    venues: ['binance'],
    providers: ['provider-alpha'],
  },
  generatedAt: '2026-05-04T06:21:00.000Z',
  source: 'mock',
};

describe('F-12 operator UI screens', () => {
  beforeEach(() => {
    jest.clearAllMocks();
    document.cookie = 'token=f12-ui-test; path=/';
    mockSearchParams = '';
    lastFeedCallbacks = null;
    mockIsAuthenticated.mockResolvedValue(true);
    mockAxiosGet.mockImplementation(async (url) => {
      if (url === '/hedge-pnl') return { data: pnlDashboard };
      if (url === '/reconciliation-alerts') return { data: reconciliationResponse };
      if (url === '/manual-overrides') return { data: manualContext };
      if (url === '/policy-config') return { data: policyContext };
      if (url === '/executions/live') return { data: executionSnapshot };
      throw new Error(`Unhandled GET ${url}`);
    });
    mockAxiosPost.mockImplementation(async (url, payload) => {
      if (url === '/auth/validate-token') return { data: { 'is-valid': true } };
      if (url === '/manual-overrides') {
        return {
          data: {
            intent: {
              intentId: 'manual-intent-001',
              symbol: payload.symbol,
              targetQty: payload.targetQty,
              venueIds: payload.venueIds,
              status: 'ACCEPTED',
              riskReason: 'Manual override accepted by mock risk policy.',
              createdAt: '2026-05-04T06:22:00.000Z',
            },
            context: {
              ...manualContext,
              recent: [{ intentId: 'manual-intent-001', symbol: payload.symbol, targetQty: payload.targetQty, venueIds: payload.venueIds, status: 'ACCEPTED', createdAt: '2026-05-04T06:22:00.000Z' }],
              summary: { sourceAlerts: 1, recent: 1, accepted: 1, rejected: 0 },
            },
          },
        };
      }
      throw new Error(`Unhandled POST ${url}`);
    });
    mockAxiosPut.mockImplementation(async (url, payload) => {
      if (url === '/policy-config') {
        return {
          data: {
            ...policyContext,
            config: {
              ...payload.config,
              revision: 2,
              updatedBy: payload.updatedBy,
              updatedAt: '2026-05-04T06:22:00.000Z',
            },
            summary: {
              ...policyContext.summary,
              revision: 2,
              avgMaxSlippageBps: 9.33,
            },
            audit: [
              {
                revision: 2,
                actor: payload.updatedBy,
                reason: payload.reason,
                updatedAt: '2026-05-04T06:22:00.000Z',
                changedFields: ['maxSlippageBps'],
              },
              ...policyContext.audit,
            ],
          },
        };
      }
      throw new Error(`Unhandled PUT ${url}`);
    });
    mockGetHedgePnlDashboard.mockResolvedValue(pnlDashboard);
    mockGetReconciliationAlerts.mockResolvedValue(reconciliationResponse);
    mockGetManualOverrideContext.mockResolvedValue(manualContext);
    mockGetPolicyConfig.mockResolvedValue(policyContext);
    mockGetExecutionLiveSnapshot.mockResolvedValue(executionSnapshot);
    mockSubscribeExecutionLiveFeed.mockImplementation((callbacks) => {
      lastFeedCallbacks = callbacks;
      callbacks.onState?.('connected');
      return jest.fn();
    });
    mockCreateManualOverride.mockResolvedValue({
      intent: {
        intentId: 'manual-intent-001',
        symbol: 'ETH/USDT',
        targetQty: 3.8,
        venueIds: ['coinbase'],
        status: 'ACCEPTED',
        riskReason: 'Manual override accepted by mock risk policy.',
        createdAt: '2026-05-04T06:22:00.000Z',
      },
      context: {
        ...manualContext,
        recent: [{ intentId: 'manual-intent-001', symbol: 'ETH/USDT', targetQty: 3.8, venueIds: ['coinbase'], status: 'ACCEPTED', createdAt: '2026-05-04T06:22:00.000Z' }],
        summary: { sourceAlerts: 1, recent: 1, accepted: 1, rejected: 0 },
      },
    });
    mockUpdatePolicyConfig.mockImplementation(async (payload) => ({
      ...policyContext,
      config: {
        ...payload.config,
        revision: 2,
        updatedBy: payload.updatedBy,
        updatedAt: '2026-05-04T06:22:00.000Z',
      },
      summary: {
        ...policyContext.summary,
        revision: 2,
        avgMaxSlippageBps: 9.33,
      },
      audit: [
        {
          revision: 2,
          actor: payload.updatedBy,
          reason: payload.reason,
          updatedAt: '2026-05-04T06:22:00.000Z',
          changedFields: ['maxSlippageBps'],
        },
        ...policyContext.audit,
      ],
    }));
  });

  test('Hedge PnL dashboard renders analytics and refetches by symbol', async () => {
    render(<HedgePnlDashboard />);

    expect(await screen.findByText('Hedge PnL Dashboard')).toBeInTheDocument();
    expect((await screen.findAllByText('BTC/USDT')).length).toBeGreaterThan(0);
    expect(screen.getAllByText('binance').length).toBeGreaterThan(0);

    await userEvent.selectOptions(screen.getByLabelText('Symbol'), 'BTC/USDT');

    await waitFor(() => {
      expect(mockGetHedgePnlDashboard).toHaveBeenLastCalledWith(expect.objectContaining({
        symbol: 'BTC/USDT',
      }));
    });
  });

  test('Execution Live Feed appends a live WebSocket report and can pause', async () => {
    render(<ExecutionLiveFeed />);

    expect(await screen.findByText('Execution Live Feed')).toBeInTheDocument();
    expect((await screen.findAllByText('exec-btc-001')).length).toBeGreaterThan(0);
    await waitFor(() => expect(lastFeedCallbacks).toBeTruthy());

    act(() => {
      lastFeedCallbacks.onReport({
        ...executionSnapshot.items[0],
        sequence: 2,
        executionId: 'exec-eth-live',
        hedgeFlowId: 'hgf-eth-live',
        symbol: 'ETH/USDT',
        venueId: 'coinbase',
        live: true,
      }, {
        generatedAt: '2026-05-04T06:22:00.000Z',
        source: 'mock:execution.venue',
      });
    });

    expect((await screen.findAllByText('exec-eth-live')).length).toBeGreaterThan(0);
    expect(screen.getAllByText('live').length).toBeGreaterThan(0);
    await userEvent.click(screen.getByRole('button', { name: 'Pause' }));
    expect(await screen.findByText('paused')).toBeInTheDocument();
  });

  test('Reconciliation Alerts renders critical alert and refetches by severity', async () => {
    render(<ReconciliationAlerts />);

    expect(await screen.findByText('Reconciliation Alerts')).toBeInTheDocument();
    expect((await screen.findAllByText('HEDGE_UNDERFILL')).length).toBeGreaterThan(0);
    expect(screen.getByRole('link', { name: 'Open manual override' })).toHaveAttribute(
      'href',
      '/manual-override?sourceAlertId=HEDGE_UNDERFILL%3Ahgf-eth-003'
    );

    await userEvent.selectOptions(screen.getByLabelText('Severity'), 'critical');

    await waitFor(() => {
      expect(mockGetReconciliationAlerts).toHaveBeenLastCalledWith(expect.objectContaining({
        severity: 'critical',
      }));
    });
  });

  test('Manual Override prefills from source alert and submits ExecutionIntent', async () => {
    render(<ManualOverride />);

    expect(await screen.findByText('Manual Override')).toBeInTheDocument();
    await userEvent.selectOptions(await screen.findByLabelText('Source alert'), 'HEDGE_UNDERFILL:hgf-eth-003');
    await userEvent.click(screen.getByRole('button', { name: 'Create ExecutionIntent' }));

    await waitFor(() => {
      expect(mockCreateManualOverride).toHaveBeenCalledWith(expect.objectContaining({
        sourceAlertId: 'HEDGE_UNDERFILL:hgf-eth-003',
        symbol: 'ETH/USDT',
        targetQty: 3.8,
        venueIds: ['coinbase'],
        urgency: 'HIGH',
      }));
    });
    expect(await screen.findByText('Created manual-intent-001')).toBeInTheDocument();
  });

  test('Policy Config edits high urgency slippage and writes audit reason', async () => {
    render(<PolicyConfig />);

    expect(await screen.findByText('Policy Config')).toBeInTheDocument();
    await screen.findByDisplayValue('solver-prod-v4');
    const highSlippageInput = screen.getAllByDisplayValue('25')[1];
    await userEvent.clear(highSlippageInput);
    await userEvent.type(highSlippageInput, '5');
    await userEvent.type(screen.getByLabelText('Audit reason'), 'Tighten HIGH slippage after risk alert');
    const form = screen.getByRole('form', { name: 'solverconfig' });
    await act(async () => {
      form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
      await Promise.resolve();
    });

    await waitFor(() => {
      expect(mockUpdatePolicyConfig).toHaveBeenCalledWith(expect.objectContaining({
        reason: 'Tighten HIGH slippage after risk alert',
        updatedBy: 'operator-ui',
        config: expect.objectContaining({
          maxSlippageBps: expect.objectContaining({ HIGH: 5 }),
        }),
      }));
    });
    expect(await screen.findByText('Saved revision 2')).toBeInTheDocument();
  });
});
