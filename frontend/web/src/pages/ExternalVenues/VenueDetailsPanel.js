import React from 'react';
import { useTranslation } from 'react-i18next';
import {
  formatCompactCurrency,
  formatDateTime,
  formatDurationMs,
  formatNumber,
  formatPercent,
  recommendationTone,
  statusTone,
} from './venueUtils';

const VenueDetailsPanel = ({ venue }) => {
  const { t } = useTranslation();

  if (!venue) {
    return <div className="venues-empty-state">{t('venues.states.select')}</div>;
  }

  const bookBestBid = venue.bookBestBid ?? venue.bestBid;
  const bookBestAsk = venue.bookBestAsk ?? venue.bestAsk;
  const bookSpread = venue.bookSpread ?? venue.spread;
  const executionBestBid = venue.executionBestBid ?? venue.bestBid;
  const executionBestAsk = venue.executionBestAsk ?? venue.bestAsk;
  const executionSpread = venue.executionSpread ?? venue.spread;

  return (
    <aside className="venues-detail-panel">
      <div className="venues-detail-header">
        <div>
          <span className="venues-detail-type">{venue.venueType.toUpperCase()}</span>
          <h2>{venue.displayName}</h2>
          <p>{venue.symbol}</p>
        </div>
        <span className={`venues-badge tone-${statusTone(venue.status)}`}>
          {t(`venues.status.${venue.status}`)}
        </span>
      </div>

      <div className="venues-detail-grid">
        <div className="venues-metric-card">
          <span>{t('venues.detail.midPrice')}</span>
          <strong>{formatNumber(venue.midPrice, 2)}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.detail.bookBidAsk')}</span>
          <strong>{formatNumber(bookBestBid, 2)} / {formatNumber(bookBestAsk, 2)}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.detail.executionBidAsk')}</span>
          <strong>{formatNumber(executionBestBid, 2)} / {formatNumber(executionBestAsk, 2)}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.detail.fillRate')}</span>
          <strong>{formatPercent(venue.fillRate)}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.detail.errorRate')}</span>
          <strong>{formatPercent(venue.errorRate, 2)}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.detail.volume24h')}</span>
          <strong>{formatCompactCurrency(venue.volume24h)}</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.detail.latency')}</span>
          <strong>{formatNumber(venue.latencyMs, 0)} ms</strong>
        </div>
        <div className="venues-metric-card">
          <span>{t('venues.detail.executionQuality')}</span>
          <strong>{formatNumber(venue.executionQuality, 0)}</strong>
        </div>
      </div>

      <div className="venues-insight-card">
        <h3>{t('venues.detail.routingTitle')}</h3>
        <p>{t(`venues.recommendationText.${venue.recommendation}`)}</p>
        <div className="venues-insight-badge-row">
          <span className={`venues-badge tone-${recommendationTone(venue.recommendation)}`}>
            {t(`venues.recommendation.${venue.recommendation}`)}
          </span>
        </div>
      </div>

      <dl className="venues-facts">
        <div>
          <dt>{t('venues.detail.lastUpdate')}</dt>
          <dd>{formatDateTime(venue.updatedAt)}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.fees')}</dt>
          <dd>{venue.feesBps} bps</dd>
        </div>
        <div>
          <dt>{t('venues.detail.bookSpread')}</dt>
          <dd>{formatNumber(bookSpread, 2)}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.executionSpread')}</dt>
          <dd>{formatNumber(executionSpread, 2)}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.staleRate')}</dt>
          <dd>{formatPercent(venue.staleRate, 2)}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.freshness')}</dt>
          <dd>{formatDurationMs(venue.freshnessMs)}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.tickLot')}</dt>
          <dd>{venue.tickSize} / {venue.lotSize}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.circuitBreaker')}</dt>
          <dd>{String(venue.circuitBreakerState || '').toUpperCase()} ({venue.circuitBreakerReason || 'n/a'})</dd>
        </div>
        <div>
          <dt>{t('venues.detail.region')}</dt>
          <dd>{venue.region}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.healthScore')}</dt>
          <dd>{venue.healthScore}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.snapshotDepth')}</dt>
          <dd>{venue.bidDepthLevels || 0} / {venue.askDepthLevels || 0}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.curveConfidence')}</dt>
          <dd>{formatPercent(venue.curveConfidence, 1)}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.curveLevel')}</dt>
          <dd>{venue.observedCurveLevel || venue.requestedCurveLevel || '—'}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.curveLatency')}</dt>
          <dd>{formatDurationMs(venue.curveBuildLatencyMs)}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.curveAction')}</dt>
          <dd>{venue.curveQualityAction || '—'}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.hedgePnl')}</dt>
          <dd>{formatCompactCurrency(venue.hedgePnl)}</dd>
        </div>
        <div>
          <dt>{t('venues.detail.hedgeCount')}</dt>
          <dd>{formatNumber(venue.hedgeCount, 0)}</dd>
        </div>
      </dl>
    </aside>
  );
};

export default VenueDetailsPanel;
