import React, { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import VenueConfigFields from './VenueConfigFields';
import { buildVenueConfigPayload, hydrateVenueConfigDraft } from './venueUtils';

const VenueOperatorActionsPanel = ({
  venue,
  isSubmitting,
  actionMessage,
  onReconnect,
  onToggleVenue,
  onRoutingModeChange,
  onSaveConfig,
  onDeleteConfig,
}) => {
  const { t } = useTranslation();
  const [draft, setDraft] = useState(() => hydrateVenueConfigDraft());

  useEffect(() => {
    setDraft(hydrateVenueConfigDraft(venue?.config, venue?.venueId));
  }, [venue?.venueId, venue?.config]);

  const patchDraft = (key, value) => setDraft((prev) => ({ ...prev, [key]: value }));

  if (!venue) {
    return <div className="venues-empty-state">{t('venues.states.select')}</div>;
  }

  const isDisabled = venue.adminState === 'disabled';
  const isWatchMode = venue.routingMode === 'watch';

  return (
    <section className="venues-actions-panel">
      <div className="venues-panel-header">
        <h3>{t('venues.operator.title')}</h3>
        <p>{t('venues.operator.subtitle')}</p>
      </div>

      <div className="venues-operator-facts">
        <div>
          <span>{t('venues.operator.adminState')}</span>
          <strong>{t(`venues.operator.state.${venue.adminState}`)}</strong>
        </div>
        <div>
          <span>{t('venues.operator.routingMode')}</span>
          <strong>{t(`venues.operator.mode.${venue.routingMode}`)}</strong>
        </div>
        <div>
          <span>{t('venues.operator.reconnectCount')}</span>
          <strong>{venue.reconnectCount || 0}</strong>
        </div>
      </div>

      <div className="venues-operator-actions">
        <button type="button" className="venues-link-btn" disabled={isSubmitting} onClick={onReconnect}>
          {t('venues.operator.actions.reconnect')}
        </button>
        <button type="button" className="venues-link-btn" disabled={isSubmitting} onClick={onToggleVenue}>
          {isDisabled ? t('venues.operator.actions.enable') : t('venues.operator.actions.disable')}
        </button>
        <button
          type="button"
          className="venues-link-btn"
          disabled={isSubmitting || !isWatchMode}
          onClick={() => onRoutingModeChange('auto')}
        >
          {t('venues.operator.actions.routeNormally')}
        </button>
        <button
          type="button"
          className="venues-link-btn"
          disabled={isSubmitting || isWatchMode}
          onClick={() => onRoutingModeChange('watch')}
        >
          {t('venues.operator.actions.watchMode')}
        </button>
      </div>

      <div className="venues-operator-config">
        <h4>{t('venues.operator.configTitle')}</h4>
        <p className="venues-operator-copy">{t('venues.operator.configSubtitle')}</p>
        <VenueConfigFields draft={draft} onPatch={patchDraft} disabled={isSubmitting} />
        <div className="venues-operator-actions">
          <button
            type="button"
            className="venues-link-btn"
            disabled={isSubmitting || !onSaveConfig}
            onClick={() => onSaveConfig?.(buildVenueConfigPayload(draft))}
          >
            {t('venues.operator.actions.saveConfig')}
          </button>
          <button
            type="button"
            className="venues-link-btn"
            disabled={isSubmitting || !onDeleteConfig}
            onClick={() => onDeleteConfig?.()}
          >
            {t('venues.operator.actions.deleteConfig')}
          </button>
        </div>
      </div>

      {actionMessage ? <div className="venues-operator-feedback">{actionMessage}</div> : null}
      {venue.lastActionAt ? (
        <div className="venues-operator-last-action">
          {t('venues.operator.lastAction')}: {venue.lastAction || '—'} at {new Date(venue.lastActionAt).toLocaleString()}
        </div>
      ) : null}
    </section>
  );
};

export default VenueOperatorActionsPanel;
