import React, { useEffect, useState } from 'react';
import { getBatches } from '../../api/batchService';
import BatchDiagnosticsTable from './BatchDiagnosticsTable';

const BatchesSection = ({ t, formatBatchTime, getBatchStatusClass }) => {
  const [batches, setBatches] = useState([]);
  const [batchesLoading, setBatchesLoading] = useState(false);
  const [batchesError, setBatchesError] = useState('');

  useEffect(() => {
    let isMounted = true;

    const loadBatches = async ({ showLoader = false } = {}) => {
      if (showLoader && isMounted) setBatchesLoading(true);
      if (isMounted) setBatchesError('');
      try {
        const batchItems = await getBatches();
        if (isMounted) setBatches(batchItems);
      } catch (error) {
        if (isMounted) {
          setBatches([]);
          setBatchesError(t('profile.batches.error'));
        }
      } finally {
        if (showLoader && isMounted) setBatchesLoading(false);
      }
    };

    loadBatches({ showLoader: true });
    const intervalId = setInterval(() => {
      loadBatches();
    }, 10000);

    return () => {
      isMounted = false;
      clearInterval(intervalId);
    };
  }, [t]);

  return (
    <section className="batches-section">
      <h2 className="batches-title">{t('profile.batches.title')}</h2>
      <div className="history-table batches-table">
        <BatchDiagnosticsTable
          batches={batches}
          batchesLoading={batchesLoading}
          batchesError={batchesError}
          t={t}
          formatBatchTime={formatBatchTime}
          getBatchStatusClass={getBatchStatusClass}
        />
      </div>
    </section>
  );
};

export default BatchesSection;
