import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { isAuthenticated } from '../../api/authService';
import {
  disableVenue,
  enableVenue,
  getVenues,
  updateVenueRoutingMode,
} from '../../api/venueService';
import useInterval from '../../hooks/useInterval';
import VenuesPageShell from './VenuesPageShell';
import VenuesListTable from './VenuesListTable';
import {
  STATUS_ORDER,
  computeSummaryCards,
  filterVenues,
} from './venueUtils';
import './ExternalVenues.css';

const POLL_INTERVAL_MS = 10000;

const ExternalVenuesList = () => {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const [isAuth, setIsAuth] = useState(null);
  const [venuesResponse, setVenuesResponse] = useState({ items: [], summary: {}, generatedAt: null });
  const [isLoading, setIsLoading] = useState(true);
  const [error, setError] = useState('');
  const [activeFilter, setActiveFilter] = useState('all');
  const [search, setSearch] = useState('');
  const [pendingVenueActionId, setPendingVenueActionId] = useState('');
  const [actionMessage, setActionMessage] = useState('');

  useEffect(() => {
    const checkAuth = async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    };
    checkAuth();
  }, [navigate]);

  const loadVenues = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setIsLoading(true);
    try {
      const response = await getVenues();
      setVenuesResponse(response);
      setError('');
    } catch (err) {
      setError(t('venues.states.error'));
    } finally {
      if (showLoader) setIsLoading(false);
    }
  }, [t]);

  useEffect(() => {
    if (isAuth) {
      loadVenues({ showLoader: true });
    }
  }, [isAuth, loadVenues]);

  useInterval(() => {
    if (isAuth) {
      loadVenues();
    }
  }, isAuth ? POLL_INTERVAL_MS : null);

  const filteredVenues = useMemo(
    () => filterVenues(venuesResponse.items, activeFilter, search),
    [venuesResponse.items, activeFilter, search]
  );

  const summaryCards = computeSummaryCards(venuesResponse);

  if (isAuth === null) {
    return <div className="loading-screen">{t('auth.loading')}</div>;
  }

  const runVenueAction = async (venueId, callback, successKey) => {
    setPendingVenueActionId(venueId);
    try {
      await callback();
      setActionMessage(t(successKey));
      setError('');
      await loadVenues();
    } catch (_) {
      setActionMessage(t('venues.operator.error'));
    } finally {
      setPendingVenueActionId('');
    }
  };

  return (
    <VenuesPageShell
      title={t('venues.title')}
      subtitle={t('venues.subtitle')}
      refreshedAt={venuesResponse.generatedAt}
    >
      <section className="venues-summary-grid">
        {summaryCards.map((card) => (
          <article className={`venues-summary-card venues-summary-${card.id}`} key={card.id}>
            <div className="venues-summary-label">{t(`venues.summary.${card.id}`)}</div>
            <div className="venues-summary-value">{card.value}</div>
          </article>
        ))}
      </section>

      <section className="venues-toolbar">
        <div className="venues-filter-group">
          {STATUS_ORDER.map((status) => (
            <button
              key={status}
              type="button"
              className={`venues-filter-chip ${activeFilter === status ? 'active' : ''}`}
              onClick={() => setActiveFilter(status)}
            >
              {t(`venues.filters.${status}`)}
            </button>
          ))}
        </div>
        <label className="venues-search">
          <span>{t('venues.searchLabel')}</span>
          <input
            type="text"
            value={search}
            onChange={(event) => setSearch(event.target.value)}
            placeholder={t('venues.searchPlaceholder')}
          />
        </label>
      </section>

      <section className="venues-list-panel">
        {actionMessage ? <div className="venues-operator-feedback">{actionMessage}</div> : null}
        {isLoading ? (
          <div className="venues-empty-state">{t('venues.states.loading')}</div>
        ) : error ? (
          <div className="venues-empty-state">{error}</div>
        ) : filteredVenues.length === 0 ? (
          <div className="venues-empty-state">{t('venues.states.empty')}</div>
        ) : (
          <VenuesListTable
            venues={filteredVenues}
            onOpenVenue={(venueId) => navigate(`/venues/${encodeURIComponent(venueId)}`)}
            onToggleVenue={(venue) =>
              runVenueAction(
                venue.venueId,
                () => (venue.adminState === 'disabled' ? enableVenue(venue.venueId) : disableVenue(venue.venueId)),
                venue.adminState === 'disabled'
                  ? 'venues.operator.successEnable'
                  : 'venues.operator.successDisable'
              )
            }
            onToggleRoutingMode={(venue) =>
              runVenueAction(
                venue.venueId,
                () => updateVenueRoutingMode(
                  venue.venueId,
                  venue.routingMode === 'watch' ? 'auto' : 'watch'
                ),
                'venues.operator.successRouting'
              )
            }
            pendingVenueActionId={pendingVenueActionId}
          />
        )}
      </section>
    </VenuesPageShell>
  );
};

export default ExternalVenuesList;
