import React from 'react';
import VenueDetailsPanel from './VenueDetailsPanel';
import './ExternalVenues.css';

export default {
  title: 'Screens/ExternalVenues/VenueDetailsPanel',
  component: VenueDetailsPanel,
};

export const Connected = {
  args: {
    venue: {
      venueId: 'binance',
      displayName: 'Binance Spot',
      venueType: 'cex',
      symbol: 'BTC/USDT',
      status: 'connected',
      recommendation: 'route',
      midPrice: 68442.1,
      bestBid: 68441.5,
      bestAsk: 68442.7,
      fillRate: 0.982,
      errorRate: 0.0042,
      volume24h: 1834500000,
      latencyMs: 42,
      updatedAt: '2026-04-06T09:15:00.000Z',
      feesBps: 10,
      staleRate: 0.012,
      tickSize: 0.1,
      lotSize: 0.0001,
      region: 'Global',
      healthScore: 92,
    },
  },
};
