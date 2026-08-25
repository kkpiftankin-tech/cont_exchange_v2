const mockGet = jest.fn();
const mockPost = jest.fn();
const mockDelete = jest.fn();

jest.mock('axios', () => ({
  create: jest.fn(() => ({
    get: mockGet,
    post: mockPost,
    delete: mockDelete,
  })),
}));

describe('replayService getReplayCompare', () => {
  beforeEach(() => {
    jest.resetModules();
    mockGet.mockReset();
    mockPost.mockReset();
    mockDelete.mockReset();
  });

  test('returns normalized compare payload from API response', async () => {
    mockGet.mockResolvedValue({
      data: {
        sessionA: 'rpl-a',
        sessionB: 'rpl-b',
        compatible: true,
        metrics: [
          { key: 'sharpe', label: 'Sharpe', valueA: 1.2, valueB: 1.5, delta: 0.3, better: 'B' },
          { key: 'avgpnl', valueA: 5, valueB: 6, delta: 1 },
          { key: 'stdpnl', valueA: 2, valueB: 1.5, delta: -0.5 },
          { key: 'avgvwap', valueA: 60100, valueB: 60125, delta: 25, preference: 'neutral' },
        ],
      },
    });

    const { getReplayCompare } = require('../replayService');
    const response = await getReplayCompare({ sessionA: 'rpl-a', sessionB: 'rpl-b' });

    expect(response.compatible).toBe(true);
    expect(response.sessionA).toBe('rpl-a');
    expect(response.sessionB).toBe('rpl-b');
    expect(response.metrics).toHaveLength(4);
    expect(response.metrics[0]).toMatchObject({
      key: 'sharpe',
      valueA: 1.2,
      valueB: 1.5,
      delta: 0.3,
      better: 'B',
    });
    expect(response.metrics).toEqual(expect.arrayContaining([
      expect.objectContaining({ key: 'avgpnl', preference: 'higher', better: 'B' }),
      expect.objectContaining({ key: 'stdpnl', preference: 'lower', better: 'B' }),
      expect.objectContaining({ key: 'avgvwap', preference: 'neutral', better: 'equal' }),
    ]));
  });

  test('propagates compare API errors instead of using synthetic diff', async () => {
    mockGet.mockRejectedValue(new Error('network unavailable'));

    const { getReplayCompare } = require('../replayService');
    await expect(getReplayCompare({
      sessionA: 'rpl-2026-0412-013',
      sessionB: 'rpl-2026-0412-014',
    })).rejects.toThrow('network unavailable');
  });
});
