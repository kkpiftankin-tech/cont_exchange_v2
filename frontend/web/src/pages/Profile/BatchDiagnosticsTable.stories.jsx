import React from 'react';
import BatchDiagnosticsTable from './BatchDiagnosticsTable';
import './Profile.css';

const t = (key, options = {}) => {
  if (Object.prototype.hasOwnProperty.call(options, 'threshold')) return `${key}:${options.threshold}`;
  return key;
};

const formatBatchTime = (value) => value;
const getBatchStatusClass = (status) => `batch-status-${String(status || '').toLowerCase()}`;

const baseArgs = {
  t,
  formatBatchTime,
  getBatchStatusClass,
  batchesLoading: false,
  batchesError: '',
};

export default {
  title: 'Screens/Profile/BatchDiagnosticsTable',
  component: BatchDiagnosticsTable,
};

export const Healthy = {
  args: {
    ...baseArgs,
    batches: [
      { batchId: 'batch-ok-1', time: '2026-03-31T10:00:00Z', status: 'SUCCESS', solveTimeMs: 84, residualNorm: 0.0009 },
      { batchId: 'batch-ok-2', time: '2026-03-31T10:00:05Z', status: 'SUCCESS', solveTimeMs: 79, residualNorm: 0.0007 },
    ],
  },
};

export const AtRisk = {
  args: {
    ...baseArgs,
    batches: [
      { batchId: 'batch-risk-1', time: '2026-03-31T10:00:00Z', status: 'PARTIAL', solveTimeMs: 180, residualNorm: 0.02 },
    ],
  },
};

export const Breached = {
  args: {
    ...baseArgs,
    batches: [
      { batchId: 'batch-fail-1', time: '2026-03-31T10:00:00Z', status: 'FAILED', solveTimeMs: 640, residualNorm: 0.73 },
    ],
  },
};

export const Loading = {
  args: {
    ...baseArgs,
    batchesLoading: true,
    batches: [],
  },
};

export const ErrorState = {
  args: {
    ...baseArgs,
    batchesError: 'Failed to load batches',
    batches: [],
  },
};

export const Empty = {
  args: {
    ...baseArgs,
    batches: [],
  },
};
