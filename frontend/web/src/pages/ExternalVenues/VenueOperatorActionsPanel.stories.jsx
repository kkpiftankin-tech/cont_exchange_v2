import React from 'react';
import VenueOperatorActionsPanel from './VenueOperatorActionsPanel';
import './ExternalVenues.css';

export default {
  title: 'Screens/ExternalVenues/VenueOperatorActionsPanel',
  component: VenueOperatorActionsPanel,
};

export const Active = {
  args: {
    venue: {
      adminState: 'active',
      routingMode: 'auto',
      reconnectCount: 2,
      lastAction: 'reconnect',
      lastActionAt: '2026-04-06T09:15:00.000Z',
    },
    isSubmitting: false,
    actionMessage: 'Reconnect sent to connector.',
    onReconnect: () => {},
    onToggleVenue: () => {},
    onRoutingModeChange: () => {},
  },
};
