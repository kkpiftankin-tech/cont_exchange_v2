import React from 'react';
import { render, screen } from '@testing-library/react';
import VenueDetailsPanel from '../VenueDetailsPanel';

jest.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key) => {
      const mapping = {
        'venues.status.connected': 'Connected',
        'venues.recommendationText.route': 'This venue looks ready for routing.',
      };
      return mapping[key] || key;
    },
  }),
}));

describe('VenueDetailsPanel', () => {
  test('renders venue metrics', () => {
    render(
      <VenueDetailsPanel
        venue={{
          venueId: 'binance',
          displayName: 'Binance Spot',
          venueType: 'cex',
          symbol: 'BTC/USDT',
          status: 'connected',
          recommendation: 'route',
          midPrice: 68442.1,
          bestBid: 68441.5,
          bestAsk: 68442.7,
          fillRate: 0.98,
          errorRate: 0.01,
          volume24h: 1230000000,
          latencyMs: 42,
          updatedAt: '2026-04-06T09:15:00.000Z',
          feesBps: 10,
          staleRate: 0.02,
          tickSize: 0.1,
          lotSize: 0.0001,
          region: 'Global',
          healthScore: 92,
        }}
      />
    );

    expect(screen.getByText('Binance Spot')).toBeInTheDocument();
    expect(screen.getByText(/BTC\/USDT/)).toBeInTheDocument();
    expect(screen.getByText(/68,442.10/)).toBeInTheDocument();
  });
});
