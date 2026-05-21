import React from 'react';
import { render, screen } from '@testing-library/react';
import BatchDiagnosticsTable from '../BatchDiagnosticsTable';

const t = (key, options = {}) => {
  if (Object.prototype.hasOwnProperty.call(options, 'threshold')) {
    return `${key}:${options.threshold}`;
  }
  return key;
};

const formatBatchTime = (value) => value;
const getBatchStatusClass = (status) => `batch-status-${String(status).toLowerCase()}`;

describe('BatchDiagnosticsTable', () => {
  test('renders empty state', () => {
    render(
      <BatchDiagnosticsTable
        batches={[]}
        batchesLoading={false}
        batchesError=""
        t={t}
        formatBatchTime={formatBatchTime}
        getBatchStatusClass={getBatchStatusClass}
      />
    );

    expect(screen.getByText('profile.batches.noData')).toBeInTheDocument();
  });

  test('renders diagnostics and sla states', () => {
    render(
      <BatchDiagnosticsTable
        batches={[
          { batchId: 'ok-1', time: '2026-03-31T10:00:00Z', status: 'SUCCESS', solveTimeMs: 80, residualNorm: 0.0008 },
          { batchId: 'risk-1', time: '2026-03-31T10:00:10Z', status: 'PARTIAL', solveTimeMs: 150, residualNorm: 0.03 },
          { batchId: 'fail-1', time: '2026-03-31T10:00:20Z', status: 'FAILED', solveTimeMs: 700, residualNorm: 0.4 },
        ]}
        batchesLoading={false}
        batchesError=""
        t={t}
        formatBatchTime={formatBatchTime}
        getBatchStatusClass={getBatchStatusClass}
      />
    );

    expect(screen.getByText('ok-1')).toBeInTheDocument();
    expect(screen.getByText('risk-1')).toBeInTheDocument();
    expect(screen.getByText('fail-1')).toBeInTheDocument();

    expect(screen.getByText('profile.batches.slaStates.ok')).toBeInTheDocument();
    expect(screen.getByText('profile.batches.slaStates.at-risk')).toBeInTheDocument();
    expect(screen.getByText('profile.batches.slaStates.breached')).toBeInTheDocument();

    expect(screen.getByText('profile.batches.diagnostics.ok')).toBeInTheDocument();
    expect(screen.getByText('profile.batches.diagnostics.solveTimeRisk:120; profile.batches.diagnostics.residualRisk:0.01')).toBeInTheDocument();
    expect(screen.getByText('profile.batches.diagnostics.solverFailed; profile.batches.diagnostics.solveTimeBreached:500; profile.batches.diagnostics.residualBreached:0.1')).toBeInTheDocument();
  });
});
