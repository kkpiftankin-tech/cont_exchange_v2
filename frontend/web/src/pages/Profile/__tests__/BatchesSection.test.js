import React from 'react';
import { render, screen, waitFor, act } from '@testing-library/react';
import BatchesSection from '../BatchesSection';

const mockGetBatches = jest.fn();

jest.mock('../../../api/batchService', () => ({
  getBatches: (...args) => mockGetBatches(...args),
}));

const t = (key, options = {}) => {
  if (Object.prototype.hasOwnProperty.call(options, 'threshold')) {
    return `${key}:${options.threshold}`;
  }
  return key;
};

const formatBatchTime = (value) => value;
const getBatchStatusClass = (status) => `batch-status-${String(status || '').toLowerCase()}`;

describe('BatchesSection polling updates', () => {
  beforeEach(() => {
    jest.useFakeTimers();
    mockGetBatches.mockReset();
  });

  afterEach(() => {
    jest.clearAllTimers();
    jest.useRealTimers();
  });

  test('requests new batches and updates UI every 10 seconds', async () => {
    mockGetBatches
      .mockResolvedValueOnce([
        { batchId: 'batch-old', time: '2026-04-01T10:00:00Z', status: 'SUCCESS', solveTimeMs: 82, residualNorm: 0.0009 },
      ])
      .mockResolvedValueOnce([
        { batchId: 'batch-old', time: '2026-04-01T10:00:00Z', status: 'SUCCESS', solveTimeMs: 82, residualNorm: 0.0009 },
        { batchId: 'batch-new', time: '2026-04-01T10:00:10Z', status: 'PARTIAL', solveTimeMs: 180, residualNorm: 0.02 },
      ]);

    let unmount;
    await act(async () => {
      const rendered = render(
        <BatchesSection
          t={t}
          formatBatchTime={formatBatchTime}
          getBatchStatusClass={getBatchStatusClass}
        />
      );
      unmount = rendered.unmount;
      await Promise.resolve();
    });

    await waitFor(() => {
      expect(mockGetBatches).toHaveBeenCalledTimes(1);
    });

    expect(await screen.findByText('batch-old')).toBeInTheDocument();
    expect(screen.queryByText('batch-new')).not.toBeInTheDocument();

    await act(async () => {
      jest.advanceTimersByTime(10000);
      await Promise.resolve();
    });

    await waitFor(() => {
      expect(mockGetBatches).toHaveBeenCalledTimes(2);
    });

    expect(screen.getByText('batch-new')).toBeInTheDocument();
    expect(
      screen.getByText('profile.batches.diagnostics.solveTimeRisk:120; profile.batches.diagnostics.residualRisk:0.01')
    ).toBeInTheDocument();

    unmount();
  });
});
