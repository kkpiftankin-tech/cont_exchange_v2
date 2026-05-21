import {
  formatResidualNorm,
  formatSolveTime,
  getBatchDiagnostics,
  getBatchRowClass,
  parseNumber,
} from '../batchDiagnostics';

const t = (key, options = {}) => {
  if (Object.prototype.hasOwnProperty.call(options, 'threshold')) {
    return `${key}:${options.threshold}`;
  }
  return key;
};

describe('batchDiagnostics', () => {
  test('parseNumber handles invalid values', () => {
    expect(parseNumber('10')).toBe(10);
    expect(parseNumber('abc')).toBeNull();
    expect(parseNumber(undefined)).toBeNull();
  });

  test('format helpers render stable output', () => {
    expect(formatSolveTime(42)).toBe('42 ms');
    expect(formatSolveTime('foo')).toBe('—');
    expect(formatResidualNorm(0)).toBe('0');
    expect(formatResidualNorm(0.5)).toBe('0.5000');
    expect(formatResidualNorm(0.00001)).toBe('1.00e-5');
  });

  test('returns breached when solver failed', () => {
    const result = getBatchDiagnostics(
      { status: 'FAILED', solveTimeMs: 50, residualNorm: 0.001 },
      t
    );
    expect(result.slaState).toBe('breached');
    expect(result.reasons).toContain('profile.batches.diagnostics.solverFailed');
  });

  test('returns at-risk for warn-level metrics', () => {
    const result = getBatchDiagnostics(
      { status: 'SUCCESS', solveTimeMs: 200, residualNorm: 0.02 },
      t
    );
    expect(result.slaState).toBe('at-risk');
    expect(result.reasons).toContain('profile.batches.diagnostics.solveTimeRisk:120');
    expect(result.reasons).toContain('profile.batches.diagnostics.residualRisk:0.01');
  });

  test('returns ok for healthy batch', () => {
    const result = getBatchDiagnostics(
      { status: 'SUCCESS', solveTimeMs: 40, residualNorm: 0.001 },
      t
    );
    expect(result.slaState).toBe('ok');
    expect(result.reasons).toHaveLength(0);
  });

  test('getBatchRowClass maps state to row class', () => {
    expect(getBatchRowClass('ok')).toBe('batch-row-ok');
    expect(getBatchRowClass('at-risk')).toBe('batch-row-at-risk');
    expect(getBatchRowClass('breached')).toBe('batch-row-breached');
  });
});
