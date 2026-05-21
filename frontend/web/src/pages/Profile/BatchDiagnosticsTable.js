import React from 'react';
import {
  formatResidualNorm,
  formatSolveTime,
  getBatchDiagnostics,
  getBatchRowClass,
} from './batchDiagnostics';

const BatchDiagnosticsTable = ({
  batches,
  batchesLoading,
  batchesError,
  t,
  formatBatchTime,
  getBatchStatusClass,
}) => {
  if (batchesLoading) return <div className="loading">{t('profile.batches.loading')}</div>;
  if (batchesError) return <div className="no-data">{batchesError}</div>;
  if (!batches || batches.length === 0) return <div className="no-data">{t('profile.batches.noData')}</div>;

  return (
    <>
      <div className="batch-table-header">
        <div>{t('profile.batches.id')}</div>
        <div>{t('profile.batches.time')}</div>
        <div>{t('profile.batches.status')}</div>
        <div>{t('profile.batches.solveTimeMs')}</div>
        <div>{t('profile.batches.residualNorm')}</div>
        <div>{t('profile.batches.sla')}</div>
        <div>{t('profile.batches.diagnosticsTitle')}</div>
      </div>
      <div className="table-body">
        {batches.map((batch) => {
          const { slaState, reasons } = getBatchDiagnostics(batch, t);
          return (
            <div className={`batch-table-row ${getBatchRowClass(slaState)}`} key={batch.batchId}>
              <div>{batch.batchId}</div>
              <div>{formatBatchTime(batch.time)}</div>
              <div className={`batch-status ${getBatchStatusClass(batch.status)}`}>{batch.status}</div>
              <div>{formatSolveTime(batch.solveTimeMs)}</div>
              <div>{formatResidualNorm(batch.residualNorm)}</div>
              <div>
                <span className={`batch-sla-badge batch-sla-${slaState}`}>
                  {t(`profile.batches.slaStates.${slaState}`)}
                </span>
              </div>
              <div className="batch-diagnostics-cell">
                {reasons.length > 0 ? reasons.join('; ') : t('profile.batches.diagnostics.ok')}
              </div>
            </div>
          );
        })}
      </div>
    </>
  );
};

export default BatchDiagnosticsTable;
