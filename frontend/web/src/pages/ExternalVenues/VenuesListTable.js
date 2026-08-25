import React from 'react';
import { useTranslation } from 'react-i18next';
import {
  formatCompactCurrency,
  formatNumber,
  recommendationTone,
  statusTone,
} from './venueUtils';

const VenuesListTable = ({
  venues,
  onOpenVenue,
  onToggleVenue,
  onToggleRoutingMode,
  pendingVenueActionId,
}) => {
  const { t } = useTranslation();

  return (
    <div className="venues-table">
      <div className="venues-table-head venues-table-head-list">
        <div>{t('venues.table.venue')}</div>
        <div>{t('venues.table.market')}</div>
        <div>{t('venues.table.status')}</div>
        <div>{t('venues.table.health')}</div>
        <div>{t('venues.table.executionQuality')}</div>
        <div>{t('venues.table.latency')}</div>
        <div>{t('venues.table.spread')}</div>
        <div>{t('venues.table.volume')}</div>
        <div>{t('venues.table.action')}</div>
        <div>{t('venues.table.controls')}</div>
        <div>{t('venues.table.details')}</div>
      </div>
      <div className="venues-table-body">
        {venues.map((venue) => {
          const isPending = pendingVenueActionId === venue.venueId;
          const isDisabled = venue.adminState === 'disabled';
          const isWatchMode = venue.routingMode === 'watch';

          return (
            <div key={venue.venueId} className="venues-row venues-row-list">
              <div className="venues-venue-cell">
                <strong>{venue.displayName}</strong>
                <span>{venue.venueId}</span>
              </div>
              <div className="venues-market-cell">
                <strong>{venue.symbol}</strong>
                <span>{venue.region}</span>
              </div>
              <div>
                <span className={`venues-badge tone-${statusTone(venue.status)}`}>
                  {t(`venues.status.${venue.status}`)}
                </span>
              </div>
              <div className="venues-health-cell">
                <strong>{venue.healthScore}</strong>
                <div className="venues-health-bar">
                  <span style={{ width: `${venue.healthScore}%` }} />
                </div>
              </div>
              <div>{formatNumber(venue.executionQuality, 0)}</div>
              <div>{formatNumber(venue.latencyMs, 0)} ms</div>
              <div>{formatNumber(venue.executionSpread ?? venue.spread, 2)}</div>
              <div>{formatCompactCurrency(venue.volume24h)}</div>
              <div>
                <span className={`venues-badge tone-${recommendationTone(venue.recommendation)}`}>
                  {t(`venues.recommendation.${venue.recommendation}`)}
                </span>
              </div>
              <div className="venues-row-controls">
                <button
                  type="button"
                  className="venues-link-btn"
                  disabled={isPending}
                  onClick={() => onToggleVenue?.(venue)}
                >
                  {isDisabled ? t('venues.operator.actions.enable') : t('venues.operator.actions.disable')}
                </button>
                <button
                  type="button"
                  className="venues-link-btn"
                  disabled={isPending || isDisabled}
                  onClick={() => onToggleRoutingMode?.(venue)}
                >
                  {isWatchMode ? t('venues.operator.actions.routeNormally') : t('venues.operator.actions.watchMode')}
                </button>
              </div>
              <div>
                <button
                  type="button"
                  className="venues-link-btn"
                  onClick={() => onOpenVenue(venue.venueId)}
                >
                  {t('venues.actions.openDetails')}
                </button>
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
};

export default VenuesListTable;
