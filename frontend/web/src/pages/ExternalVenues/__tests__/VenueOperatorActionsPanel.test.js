import React from 'react';
import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import VenueOperatorActionsPanel from '../VenueOperatorActionsPanel';

jest.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key) => {
      const mapping = {
        'venues.operator.actions.reconnect': 'Force reconnect',
        'venues.operator.actions.disable': 'Disable venue',
        'venues.operator.actions.watchMode': 'Watch mode',
        'venues.operator.state.active': 'Active',
        'venues.operator.mode.auto': 'Auto',
      };
      return mapping[key] || key;
    },
  }),
}));

describe('VenueOperatorActionsPanel', () => {
  test('fires operator action callbacks', async () => {
    const onReconnect = jest.fn();
    const onToggleVenue = jest.fn();
    const onRoutingModeChange = jest.fn();

    render(
      <VenueOperatorActionsPanel
        venue={{
          adminState: 'active',
          routingMode: 'auto',
          reconnectCount: 2,
          lastAction: 'reconnect',
          lastActionAt: '2026-04-06T09:15:00.000Z',
        }}
        isSubmitting={false}
        actionMessage=""
        onReconnect={onReconnect}
        onToggleVenue={onToggleVenue}
        onRoutingModeChange={onRoutingModeChange}
      />
    );

    await userEvent.click(screen.getByRole('button', { name: /reconnect/i }));
    await userEvent.click(screen.getByRole('button', { name: /disable/i }));
    await userEvent.click(screen.getByRole('button', { name: /watch/i }));

    expect(onReconnect).toHaveBeenCalled();
    expect(onToggleVenue).toHaveBeenCalled();
    expect(onRoutingModeChange).toHaveBeenCalledWith('watch');
  });
});
