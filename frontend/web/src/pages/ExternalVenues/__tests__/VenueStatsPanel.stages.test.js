import React from 'react';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import VenueStatsPanel from '../VenueStatsPanel';

// Этап 0 (EXP-2026-10-15-exchange-requests): панель показывает тайминги стадий опроса из DTO
// /api/venues/stats и не выдумывает значений там, где venues их не отдал.
jest.mock('../../../api/venueService', () => ({
  getVenueStats: jest.fn(),
  getVenueStaleConfig: jest.fn().mockResolvedValue({ venue_stale_ms: 180000 }),
  setVenueStaleConfig: jest.fn(),
}));
const { getVenueStats } = require('../../../api/venueService');

const stats = {
  window_s: 600,
  venues: [
    {
      venue_id: 'binance', avg_period_ms: 24000, age_ms: 5000, pair_count: 2, cycle_ms: 19000, stages_available: true,
      pairs: [
        { symbol: 'BTC/USDT', period_ms: 24000, age_ms: 5000, samples: 20,
          stages: { rest_depth_ms: 980, rest_ticker_ms: 0, rest_trades_ms: 0, rest_requests: 1, ticker_state: 1,
            lock_wait_ms: 2, raw_publish_ms: 3, snapshot_ms: 40, curve_ms: 90, total_ms: 1115, book_age_ms: 7000 } },
        { symbol: 'ETH/USDT', period_ms: 24000, age_ms: 6000, samples: 20, stages: null },
      ],
    },
    { venue_id: 'okx', avg_period_ms: 23000, age_ms: 4000, pair_count: 1, cycle_ms: null, stages_available: false,
      pairs: [{ symbol: 'BTC/USDT', period_ms: 23000, age_ms: 4000, samples: 20, stages: null }] },
  ],
};

test('показывает проход опроса, стадии пары и «—» там, где данных нет', async () => {
  getVenueStats.mockResolvedValue(stats);
  const { container } = render(<VenueStatsPanel />);
  await waitFor(() => expect(container.textContent).toMatch(/binance/));
  expect(container.textContent).toMatch(/19\.0 с/);               // проход опроса binance
  fireEvent.click(screen.getAllByText('Показать пары')[0]);
  expect(container.textContent).toMatch(/980 мс/);                 // REST стакан
  expect(container.textContent).toMatch(/кэш/);                    // тикер пропущен по кэшу
  expect(container.textContent).toMatch(/1\.1 с/);                 // итого пары 1115 мс
  expect(container.textContent).toMatch(/7\.0 с/);                 // возраст книги
  // пара без стадий и биржа без метрик venues: пустые ячейки, не нули
  const dashes = (container.textContent.match(/—/g) || []).length;
  expect(dashes).toBeGreaterThan(5);
});
