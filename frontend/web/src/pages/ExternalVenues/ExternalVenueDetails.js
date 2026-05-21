import React, { useCallback, useEffect, useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { isAuthenticated } from '../../api/authService';
import {
  deleteVenueConfig,
  disableVenue,
  enableVenue,
  getVenueById,
  getVenueCurves,
  getVenueSnapshots,
  getVenueSynthetics,
  reconnectVenue,
  upsertVenueConfig,
  updateVenueRoutingMode,
} from '../../api/venueService';
import useInterval from '../../hooks/useInterval';
import VenuesPageShell from './VenuesPageShell';
import VenueDetailsPanel from './VenueDetailsPanel';
import VenueCurvesPanel from './VenueCurvesPanel';
import VenueOperatorActionsPanel from './VenueOperatorActionsPanel';
import VenueSnapshotsPanel from './VenueSnapshotsPanel';
import VenueSyntheticsPanel from './VenueSyntheticsPanel';
import './ExternalVenues.css';

const POLL_INTERVAL_MS = 10000;

const ExternalVenueDetails = () => {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const { venueId } = useParams();

  const [isAuth, setIsAuth] = useState(null);
  const [venue, setVenue] = useState(null);
  const [curvesResponse, setCurvesResponse] = useState({ items: [], generatedAt: null });
  const [snapshotsResponse, setSnapshotsResponse] = useState({ items: [], generatedAt: null });
  const [syntheticsResponse, setSyntheticsResponse] = useState({ items: [], generatedAt: null });
  const [isLoading, setIsLoading] = useState(true);
  const [isSubmitting, setIsSubmitting] = useState(false);
  const [error, setError] = useState('');
  const [actionMessage, setActionMessage] = useState('');

  useEffect(() => {
    const checkAuth = async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    };
    checkAuth();
  }, [navigate]);

  const loadVenue = useCallback(async ({ showLoader = false } = {}) => {
    if (!venueId) return;
    if (showLoader) setIsLoading(true);
    try {
      const [venueResponse, curvesResult, snapshotsResult, syntheticsResult] = await Promise.allSettled([
        getVenueById(venueId),
        getVenueCurves(venueId, 3),
        getVenueSnapshots(venueId),
        getVenueSynthetics(venueId),
      ]);
      if (venueResponse.status !== 'fulfilled') {
        throw venueResponse.reason || new Error('venue_not_found');
      }
      setVenue(venueResponse.value);
      setError('');
      setCurvesResponse(curvesResult.status === 'fulfilled' ? curvesResult.value : { items: [], generatedAt: null });
      setSnapshotsResponse(
        snapshotsResult.status === 'fulfilled' ? snapshotsResult.value : { items: [], generatedAt: null }
      );
      setSyntheticsResponse(
        syntheticsResult.status === 'fulfilled' ? syntheticsResult.value : { items: [], generatedAt: null }
      );
    } catch (err) {
      setError(t('venues.states.notFound'));
      setVenue(null);
      setCurvesResponse({ items: [], generatedAt: null });
      setSnapshotsResponse({ items: [], generatedAt: null });
      setSyntheticsResponse({ items: [], generatedAt: null });
    } finally {
      if (showLoader) setIsLoading(false);
    }
  }, [t, venueId]);

  useEffect(() => {
    if (isAuth) {
      loadVenue({ showLoader: true });
    }
  }, [isAuth, loadVenue]);

  useInterval(() => {
    if (isAuth) {
      loadVenue();
    }
  }, isAuth ? POLL_INTERVAL_MS : null);

  const runAction = async (callback, successKey) => {
    if (!venueId) return;
    setIsSubmitting(true);
    try {
      await callback();
      setActionMessage(t(successKey));
      await loadVenue();
    } catch (err) {
      setActionMessage(t('venues.operator.error'));
    } finally {
      setIsSubmitting(false);
    }
  };

  if (isAuth === null) {
    return <div className="loading-screen">{t('auth.loading')}</div>;
  }

  return (
    <VenuesPageShell
      title={venue ? venue.displayName : t('venues.detailPage.title')}
      subtitle={venue ? t('venues.detailPage.subtitle', { symbol: venue.symbol, venueId: venue.venueId }) : t('venues.detailPage.fallbackSubtitle')}
      refreshedAt={venue?.updatedAt}
      action={
        <button type="button" className="venues-back-btn" onClick={() => navigate('/venues')}>
          {t('venues.actions.backToList')}
        </button>
      }
    >
      {isLoading ? (
        <section className="venues-detail-layout">
          <div className="venues-empty-state">{t('venues.states.loading')}</div>
        </section>
      ) : error ? (
        <section className="venues-detail-layout">
          <div className="venues-empty-state">{error}</div>
        </section>
      ) : (
        <section className="venues-detail-layout">
          <div className="venues-detail-stack">
            <VenueDetailsPanel venue={venue} />
            <VenueSnapshotsPanel venue={venue} snapshots={snapshotsResponse.items} />
            <VenueCurvesPanel curves={curvesResponse.items} />
            <VenueSyntheticsPanel synthetics={syntheticsResponse.items} />
            <VenueOperatorActionsPanel
              venue={venue}
              isSubmitting={isSubmitting}
              actionMessage={actionMessage}
              onReconnect={() => runAction(() => reconnectVenue(venueId), 'venues.operator.successReconnect')}
              onToggleVenue={() => runAction(
                () => (venue?.adminState === 'disabled' ? enableVenue(venueId) : disableVenue(venueId)),
                venue?.adminState === 'disabled' ? 'venues.operator.successEnable' : 'venues.operator.successDisable'
              )}
              onRoutingModeChange={(mode) =>
                runAction(() => updateVenueRoutingMode(venueId, mode), 'venues.operator.successRouting')
              }
              onSaveConfig={(payload) =>
                runAction(() => upsertVenueConfig(venueId, payload), 'venues.operator.successConfig')
              }
              onDeleteConfig={() =>
                runAction(() => deleteVenueConfig(venueId), 'venues.operator.successDeleteConfig')
              }
            />
          </div>
        </section>
      )}
    </VenuesPageShell>
  );
};

export default ExternalVenueDetails;
