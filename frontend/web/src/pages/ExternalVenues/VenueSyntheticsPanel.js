import React, { useMemo } from 'react';
import { useTranslation } from 'react-i18next';
import {
  formatDateTime,
  formatDurationMs,
  formatNumber,
} from './venueUtils';

const VenueSyntheticsPanel = ({ synthetics }) => {
  const { t } = useTranslation();

  const orderedSynthetics = useMemo(
    () => [...(Array.isArray(synthetics) ? synthetics : [])]
      .sort((left, right) => new Date(right.createdAt).getTime() - new Date(left.createdAt).getTime()),
    [synthetics]
  );
  const activeSynthetics = orderedSynthetics.filter((row) => String(row.status || '').toLowerCase() === 'active');

  if (orderedSynthetics.length === 0) {
    return <div className="venues-empty-state">{t('venues.synthetics.empty')}</div>;
  }

  return (
    <section className="venues-synthetics-panel">
      <div className="venues-panel-header">
        <h3>{t('venues.synthetics.title')}</h3>
        <p>{t('venues.synthetics.subtitle')}</p>
      </div>

      <div className="venues-detail-grid">
        <div className="venues-metric-card">
          <span>{t('venues.synthetics.total')}</span>
          <strong>{orderedSynthetics.length}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.synthetics.active')}</span>
          <strong>{activeSynthetics.length}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.synthetics.latestRate')}</span>
          <strong>{formatNumber(orderedSynthetics[0]?.rateQty, 6)}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.synthetics.latestMaxQty')}</span>
          <strong>{formatNumber(orderedSynthetics[0]?.maxQty, 6)}</strong>
        </div>
      </div>

      <div className="venues-mini-table">
        <div className="venues-mini-table-head venues-mini-table-head-synthetics">
          <div>{t('venues.synthetics.table.time')}</div>
          <div>{t('venues.synthetics.table.side')}</div>
          <div>{t('venues.synthetics.table.band')}</div>
          <div>{t('venues.synthetics.table.speed')}</div>
          <div>{t('venues.synthetics.table.status')}</div>
          <div>{t('venues.synthetics.table.links')}</div>
        </div>
        <div className="venues-mini-table-body">
          {orderedSynthetics.map((row) => (
            <div className="venues-mini-table-row venues-mini-table-row-synthetics" key={row.syntheticId}>
              <div>
                <strong>{formatDateTime(row.createdAt)}</strong>
                <span>{formatDurationMs(Math.max(0, new Date(row.expiresAt).getTime() - Date.now()))}</span>
              </div>
              <div>{row.side || '—'}</div>
              <div>{formatNumber(row.priceLow, 2)} .. {formatNumber(row.priceHigh, 2)}</div>
              <div>{formatNumber(row.rateQty, 6)} / {formatNumber(row.maxQty, 6)}</div>
              <div>{row.status || '—'}</div>
              <div>
                <strong>{row.curveId || '—'}</strong>
                <span>{row.snapshotId || row.orderId || '—'}</span>
              </div>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
};

export default VenueSyntheticsPanel;
