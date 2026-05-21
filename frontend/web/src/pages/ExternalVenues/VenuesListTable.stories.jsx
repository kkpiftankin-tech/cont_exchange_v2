import React from 'react';
import VenuesListTable from './VenuesListTable';
import './ExternalVenues.css';

export default {
  title: 'Screens/ExternalVenues/VenuesListTable',
  component: VenuesListTable,
};

export const MixedStatuses = {
  args: {
    venues: [
      {
        venueId: 'binance',
        displayName: 'Binance Spot',
        symbol: 'BTC/USDT',
        region: 'Global',
        status: 'connected',
        healthScore: 92,
        latencyMs: 42,
        spread: 1.2,
        volume24h: 1834500000,
        recommendation: 'route',
      },
      {
        venueId: 'coinbase',
        displayName: 'Coinbase Advanced',
        symbol: 'BTC/USD',
        region: 'US',
        status: 'stale',
        healthScore: 76,
        latencyMs: 141,
        spread: 3.8,
        volume24h: 864000000,
        recommendation: 'watch',
      },
    ],
    onOpenVenue: () => {},
  },
};
