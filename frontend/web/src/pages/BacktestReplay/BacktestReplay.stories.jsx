import React from 'react';
import { MemoryRouter } from 'react-router-dom';
import BacktestReplay from './BacktestReplay';

const STORAGE_KEY = 'cont.replay.sessions.v2';

const baseCompleted = {
  sessionid: 'rpl-2026-0412-013',
  name: 'BTC mean reversion',
  status: 'completed',
  progressbatches: 1200,
  totalbatches: 1200,
  progress: 100,
  createdat: '2026-04-12T15:20:00.000Z',
  daterangefrom: '2026-04-01',
  daterangeto: '2026-04-07',
  instrument: 'BTC/USDT',
  solverconfigid: 'solver-prod-v4',
  risklimitsid: 'risk-standard',
  feemodel: 'maker-taker-spot',
  rewardmode: 'incrementalPnL',
  strategy: {
    name: 'mean-reversion-btc',
    instruments: ['BTC/USDT'],
  },
};

const baseRunning = {
  sessionid: 'rpl-2026-0412-014',
  name: 'Fee stress check',
  status: 'running',
  progressbatches: 744,
  totalbatches: 1200,
  progress: 62,
  createdat: '2026-04-12T16:05:00.000Z',
  daterangefrom: '2026-04-03',
  daterangeto: '2026-04-08',
  instrument: 'BTC/USDT',
  solverconfigid: 'solver-low-latency',
  risklimitsid: 'risk-standard',
  feemodel: 'fee-stress-high',
  rewardmode: '-IS',
  strategy: {
    name: 'fee-stress',
    instruments: ['BTC/USDT'],
  },
};

const baseFailed = {
  sessionid: 'rpl-2026-0411-021',
  name: 'Risk tight limits',
  status: 'failed',
  progressbatches: 492,
  totalbatches: 1200,
  progress: 41,
  createdat: '2026-04-11T18:43:00.000Z',
  daterangefrom: '2026-03-24',
  daterangeto: '2026-03-31',
  instrument: 'ETH/USDT',
  solverconfigid: 'solver-audit-safe',
  risklimitsid: 'risk-tight',
  feemodel: 'maker-taker-spot',
  rewardmode: '-IS',
  errordetails: 'Solver diverged at batch 1203 and tolerance was exceeded.',
  errorcode: 'solver_error',
  partialsummary: true,
  strategy: {
    name: 'risk-tight-limits',
    instruments: ['ETH/USDT'],
  },
};

const baseCancelled = {
  sessionid: 'rpl-2026-0410-005',
  name: 'Cancelled no-data audit',
  status: 'cancelled',
  progressbatches: 16,
  totalbatches: 80,
  progress: 20,
  createdat: '2026-04-10T08:00:00.000Z',
  daterangefrom: '2026-03-01',
  daterangeto: '2026-03-01',
  instrument: 'SOL/USDT',
  solverconfigid: 'solver-prod-v4',
  risklimitsid: 'risk-observer',
  feemodel: 'zero-fee-control',
  rewardmode: 'incrementalPnL',
  errordetails: 'Replay cancelled by operator after sparse market data warning.',
  errorcode: 'cancelled_by_user',
  partialsummary: true,
  strategy: {
    name: 'cancelled-sol-audit',
    instruments: ['SOL/USDT'],
  },
};

const basePending = {
  sessionid: 'rpl-2026-0426-999',
  name: 'Queued replay candidate',
  status: 'pending',
  progressbatches: 0,
  totalbatches: 96,
  progress: 0,
  createdat: '2026-04-26T12:00:00.000Z',
  daterangefrom: '2026-04-20',
  daterangeto: '2026-04-21',
  instrument: 'BTC/USDT',
  solverconfigid: 'solver-prod-v4',
  risklimitsid: 'risk-standard',
  feemodel: 'maker-taker-spot',
  rewardmode: 'incrementalPnL',
  strategy: {
    name: 'queued-btc-replay',
    instruments: ['BTC/USDT'],
  },
};

function withReplaySessions(sessions) {
  return (Story) => {
    if (typeof window !== 'undefined') {
      window.localStorage.setItem(STORAGE_KEY, JSON.stringify(sessions));
      try {
        window.WebSocket = undefined;
      } catch (error) {
        // Ignore if the environment does not allow overriding WebSocket.
      }
      try {
        window.EventSource = undefined;
      } catch (error) {
        // Ignore if the environment does not allow overriding EventSource.
      }
    }

    return (
      <MemoryRouter>
        <Story />
      </MemoryRouter>
    );
  };
}

const meta = {
  title: 'Screens/Replay/BacktestReplay',
  component: BacktestReplay,
  parameters: {
    layout: 'fullscreen',
  },
};

export default meta;

export const PendingQueue = {
  decorators: [withReplaySessions([basePending, baseCompleted, baseRunning])],
};

export const RunningLive = {
  decorators: [withReplaySessions([baseRunning, baseCompleted, baseFailed])],
};

export const CompletedResults = {
  decorators: [withReplaySessions([baseCompleted, baseRunning, baseFailed])],
};

export const FailedWithPartialSummary = {
  decorators: [withReplaySessions([baseFailed, baseCompleted, baseCancelled])],
};

export const CancelledWithPartialSummary = {
  decorators: [withReplaySessions([baseCancelled, baseCompleted, baseRunning])],
};

export const CompareAndAuditWorkspace = {
  decorators: [withReplaySessions([baseCompleted, baseRunning, baseFailed, baseCancelled])],
};
