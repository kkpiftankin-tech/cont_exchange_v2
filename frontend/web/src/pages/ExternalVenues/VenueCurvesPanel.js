import React from 'react';
import { useTranslation } from 'react-i18next';
import { formatNumber, formatPercent } from './venueUtils';

const VenueCurvesPanel = ({ curves }) => {
  const { t } = useTranslation();

  if (!curves || curves.length === 0) {
    return <div className="venues-empty-state">{t('venues.curves.empty')}</div>;
  }

  return (
    <section className="venues-curves-panel">
      <div className="venues-panel-header">
        <h3>{t('venues.curves.title')}</h3>
        <p>{t('venues.curves.subtitle')}</p>
      </div>
      <div className="venues-curves-grid">
        {curves.map((curve) => (
          <article className="venues-curve-card" key={curve.curveId}>
            <div className="venues-curve-header">
              <div>
                <span className="venues-detail-type">{curve.level}</span>
                <h4>{t(`venues.curves.side.${curve.side}`)}</h4>
              </div>
              <div className="venues-curve-confidence">{t('venues.curves.confidence')}: {formatPercent(curve.confidence, 0)}</div>
            </div>
            <div className="venues-curve-metrics">
              <div><span>tau</span><strong>{curve.tauSec}s</strong></div>
              <div><span>epsilon1</span><strong>{formatNumber(curve.epsilon1, 4)}</strong></div>
              <div><span>epsilon2</span><strong>{formatNumber(curve.epsilon2, 4)}</strong></div>
              <div><span>epsilon3</span><strong>{formatNumber(curve.epsilon3, 4)}</strong></div>
            </div>
            <div className="venues-curve-points">
              {curve.qGrid.map((q, index) => (
                <div className="venues-curve-point" key={`${curve.curveId}-${q}`}>
                  <span>q={formatNumber(q, 2)}</span>
                  <strong>p={formatNumber(curve.pOfQ[index], 2)}</strong>
                  <small>L(v)={formatNumber(curve.lOfV[index], 4)}</small>
                </div>
              ))}
            </div>
          </article>
        ))}
      </div>
    </section>
  );
};

export default VenueCurvesPanel;
