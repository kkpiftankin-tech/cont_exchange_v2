export const SOLVER_SLA = {
  solveTimeMsWarn: 120,
  solveTimeMsFail: 500,
  residualNormWarn: 0.01,
  residualNormFail: 0.1,
};

export const parseNumber = (value) => {
  const num = Number(value);
  return Number.isFinite(num) ? num : null;
};

export const formatResidualNorm = (value) => {
  const num = parseNumber(value);
  if (num === null) return '—';
  if (num === 0) return '0';
  if (num < 0.001) return num.toExponential(2);
  return num.toFixed(4);
};

export const formatSolveTime = (value) => {
  const num = parseNumber(value);
  if (num === null) return '—';
  return `${num} ms`;
};

const translate = (t, key, options = {}) => {
  if (typeof t === 'function') return t(key, options);
  return key;
};

export const getBatchDiagnostics = (batch, t, sla = SOLVER_SLA) => {
  const status = String(batch?.status || '').toUpperCase();
  const solveTimeMs = parseNumber(batch?.solveTimeMs);
  const residualNorm = parseNumber(batch?.residualNorm);
  const isSolverFailed = status === 'FAILED' || status === 'ERROR';

  const hasSolveSlaFail = solveTimeMs !== null && solveTimeMs > sla.solveTimeMsFail;
  const hasSolveSlaWarn = solveTimeMs !== null && solveTimeMs > sla.solveTimeMsWarn;
  const hasResidualFail = residualNorm !== null && residualNorm > sla.residualNormFail;
  const hasResidualWarn = residualNorm !== null && residualNorm > sla.residualNormWarn;

  const reasons = [];
  if (isSolverFailed) reasons.push(translate(t, 'profile.batches.diagnostics.solverFailed'));
  if (hasSolveSlaFail) reasons.push(translate(t, 'profile.batches.diagnostics.solveTimeBreached', { threshold: sla.solveTimeMsFail }));
  else if (hasSolveSlaWarn) reasons.push(translate(t, 'profile.batches.diagnostics.solveTimeRisk', { threshold: sla.solveTimeMsWarn }));
  if (hasResidualFail) reasons.push(translate(t, 'profile.batches.diagnostics.residualBreached', { threshold: sla.residualNormFail }));
  else if (hasResidualWarn) reasons.push(translate(t, 'profile.batches.diagnostics.residualRisk', { threshold: sla.residualNormWarn }));

  let slaState = 'ok';
  if (isSolverFailed || hasSolveSlaFail || hasResidualFail) slaState = 'breached';
  else if (hasSolveSlaWarn || hasResidualWarn || status === 'PARTIAL') slaState = 'at-risk';

  return {
    slaState,
    reasons,
  };
};

export const getBatchRowClass = (slaState) => `batch-row-${slaState}`;
