import React from 'react';
import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import VenuesListTable from '../VenuesListTable';

jest.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key) => {
      const mapping = {
        'venues.actions.openDetails': 'Open',
        'venues.status.connected': 'Connected',
        'venues.recommendation.route': 'Route',
        'venues.operator.actions.disable': 'Disable venue',
        'venues.operator.actions.enable': 'Enable venue',
        'venues.operator.actions.watchMode': 'Watch mode',
        'venues.operator.actions.routeNormally': 'Route normally',
        'venues.table.controls': 'Controls',
      };
      return mapping[key] || key;
    },
  }),
}));

describe('VenuesListTable', () => {
  test('renders venues, exposes row controls, and opens details on click', async () => {
    const onOpenVenue = jest.fn();
    const onToggleVenue = jest.fn();
    const onToggleRoutingMode = jest.fn();

    render(
      <VenuesListTable
        venues={[
          {
            venueId: 'binance',
            displayName: 'Binance Spot',
            symbol: 'BTC/USDT',
            region: 'Global',
            status: 'connected',
            healthScore: 91,
            latencyMs: 42,
            spread: 1.2,
            volume24h: 1230000000,
            recommendation: 'route',
            adminState: 'active',
            routingMode: 'auto',
          },
        ]}
        onOpenVenue={onOpenVenue}
        onToggleVenue={onToggleVenue}
        onToggleRoutingMode={onToggleRoutingMode}
        pendingVenueActionId=""
      />
    );

    expect(screen.getByText('Binance Spot')).toBeInTheDocument();
    await userEvent.click(screen.getByRole('button', { name: /disable venue/i }));
    await userEvent.click(screen.getByRole('button', { name: /watch mode/i }));
    await userEvent.click(screen.getByRole('button', { name: /open/i }));
    expect(onToggleVenue).toHaveBeenCalledWith(expect.objectContaining({ venueId: 'binance' }));
    expect(onToggleRoutingMode).toHaveBeenCalledWith(expect.objectContaining({ venueId: 'binance' }));
    expect(onOpenVenue).toHaveBeenCalledWith('binance');
  });
});
