import {
  computeSummaryCards,
  filterVenues,
  formatPercent,
  recommendationTone,
  statusTone,
} from '../venueUtils';

describe('venueUtils', () => {
  test('computes summary cards from API response', () => {
    const cards = computeSummaryCards({
      summary: {
        total: 5,
        healthy: 3,
        stale: 1,
        disabled: 1,
      },
    });

    expect(cards).toEqual([
      { id: 'total', value: 5 },
      { id: 'healthy', value: 3 },
      { id: 'stale', value: 1 },
      { id: 'disabled', value: 1 },
    ]);
  });

  test('filters by status and search query', () => {
    const items = [
      { venueId: 'binance', displayName: 'Binance Spot', symbol: 'BTC/USDT', venueType: 'cex', region: 'Global', status: 'connected' },
      { venueId: 'uniswap_v3', displayName: 'Uniswap v3', symbol: 'WBTC/USDC', venueType: 'dex', region: 'On-chain', status: 'stale' },
    ];

    expect(filterVenues(items, 'stale', '')).toHaveLength(1);
    expect(filterVenues(items, 'all', 'uni')).toHaveLength(1);
    expect(filterVenues(items, 'connected', 'wbtc')).toHaveLength(0);
  });

  test('formats percentages and maps tones', () => {
    expect(formatPercent(0.125)).toBe('12.5%');
    expect(statusTone('connected')).toBe('good');
    expect(statusTone('disconnected')).toBe('bad');
    expect(recommendationTone('watch')).toBe('warn');
  });
});
