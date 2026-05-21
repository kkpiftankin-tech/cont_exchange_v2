import React, { useMemo } from 'react';
import { useTranslation } from 'react-i18next';
import {
  formatCompactCurrency,
  formatDateTime,
  formatDurationMs,
  formatNumber,
} from './venueUtils';

const MAX_DEPTH_ROWS = 8;
const MAX_HISTORY_ROWS = 6;

const VenueSnapshotsPanel = ({ venue, snapshots }) => {
  const { t } = useTranslation();

  const orderedSnapshots = useMemo(
    () => [...(Array.isArray(snapshots) ? snapshots : [])]
      .sort((left, right) => new Date(right.updatedAt).getTime() - new Date(left.updatedAt).getTime()),
    [snapshots]
  );
  const latestSnapshot = orderedSnapshots[0] || null;

  if (!venue || !latestSnapshot) {
    return <div className="venues-empty-state">{t('venues.snapshots.empty')}</div>;
  }

  const freshnessMs = Math.max(0, Date.now() - new Date(latestSnapshot.updatedAt).getTime());

  return (
    <section className="venues-snapshots-panel">
      <div className="venues-panel-header">
        <h3>{t('venues.snapshots.title')}</h3>
        <p>{t('venues.snapshots.subtitle')}</p>
      </div>

      <div className="venues-detail-grid">
        <div className="venues-metric-card">
          <span>{t('venues.snapshots.latestStatus')}</span>
          <strong>{latestSnapshot.status || '—'}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.snapshots.freshness')}</span>
          <strong>{formatDurationMs(freshnessMs)}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.snapshots.depth')}</span>
          <strong>{latestSnapshot.bidDepthLevels} / {latestSnapshot.askDepthLevels}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.snapshots.volume24hBase')}</span>
          <strong>{formatNumber(latestSnapshot.volume24hBase, 6)}</strong>
        </div>
      </div>

      <div className="venues-depth-grid">
        <article className="venues-depth-card">
          <div className="venues-depth-title">{t('venues.snapshots.bidDepth')}</div>
          {latestSnapshot.bidDepth.slice(0, MAX_DEPTH_ROWS).map((level, index) => (
            <div className="venues-depth-row" key={`bid-${level.price}-${index}`}>
              <span>{formatNumber(level.price, 2)}</span>
              <strong>{formatNumber(level.quantity, 6)}</strong>
            </div>
          ))}
        </article>

        <article className="venues-depth-card">
          <div className="venues-depth-title">{t('venues.snapshots.askDepth')}</div>
          {latestSnapshot.askDepth.slice(0, MAX_DEPTH_ROWS).map((level, index) => (
            <div className="venues-depth-row" key={`ask-${level.price}-${index}`}>
              <span>{formatNumber(level.price, 2)}</span>
              <strong>{formatNumber(level.quantity, 6)}</strong>
            </div>
          ))}
        </article>
      </div>

      <div className="venues-mini-table">
        <div className="venues-mini-table-head">
          <div>{t('venues.snapshots.table.time')}</div>
          <div>{t('venues.snapshots.table.status')}</div>
          <div>{t('venues.snapshots.table.bidAsk')}</div>
          <div>{t('venues.snapshots.table.mid')}</div>
          <div>{t('venues.snapshots.table.volume')}</div>
        </div>
        <div className="venues-mini-table-body">
          {orderedSnapshots.slice(0, MAX_HISTORY_ROWS).map((snapshot) => (
            <div className="venues-mini-table-row" key={`${snapshot.updatedAt}-${snapshot.status}`}>
              <div>{formatDateTime(snapshot.updatedAt)}</div>
              <div>{snapshot.status || '—'}</div>
              <div>{formatNumber(snapshot.bestBid, 2)} / {formatNumber(snapshot.bestAsk, 2)}</div>
              <div>{formatNumber(snapshot.midPrice, 2)}</div>
              <div>{formatCompactCurrency(snapshot.volume24h)}</div>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
};

export default VenueSnapshotsPanel;
