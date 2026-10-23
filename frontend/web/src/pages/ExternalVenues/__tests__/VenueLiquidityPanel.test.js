import React from 'react';
import { render, waitFor } from '@testing-library/react';
import VenueLiquidityPanel, { fmtTapePerMin, fmtCoef, coefColor } from '../VenueLiquidityPanel';

// ADR-082 (шаг 0): панель показывает ν и f из DTO /api/venues/liquidity-stats как есть и не выдумывает значений там,
// где измерения ещё нет (нет заявок ⇒ «—», а не 100 %).
jest.mock('../../../api/venueService', () => ({
  getVenueLiquidityStats: jest.fn(),
}));
const { getVenueLiquidityStats } = require('../../../api/venueService');

const dto = {
  venues: [
    {
      venue: 'binance',
      pairs: [
        { symbol: 'BTCUSDT', base: 'BTC', quote: 'USDT', has_tape: true, tape_quote_per_min: 1260000, tape_samples: 300,
          has_fill: true, fill_coef: 0.72, fill_orders: 120, age_ms: 4000, stale: false },
        { symbol: 'ETHBTC', base: 'ETH', quote: 'BTC', has_tape: true, tape_quote_per_min: 0.5, tape_samples: 12,
          has_fill: false, fill_coef: null, fill_orders: 0, age_ms: 3000, stale: false },
      ],
    },
    {
      venue: 'kraken',
      pairs: [
        { symbol: 'ETHUSDT', base: 'ETH', quote: 'USDT', has_tape: true, tape_quote_per_min: 750, tape_samples: 40,
          has_fill: true, fill_coef: 0.2, fill_orders: 9, age_ms: 5000, stale: false },
      ],
    },
  ],
};

test('форматы: оборот в долларах и в базовой котировке, f в процентах, «—» без данных', () => {
  expect(fmtTapePerMin(1260000, 'USDT')).toBe('$ 1.26 млн');
  expect(fmtTapePerMin(750, 'USDT')).toBe('$ 750');
  expect(fmtTapePerMin(12300, 'USD')).toBe('$ 12.3 тыс.');
  expect(fmtTapePerMin(0.5, 'BTC')).toBe('0.5000 BTC');
  expect(fmtTapePerMin(null, 'USDT')).toBe('—');
  expect(fmtCoef(0.72)).toBe('72 %');
  expect(fmtCoef(null)).toBe('—');
  expect(coefColor(0.72)).toBe('#1f9d55');
  expect(coefColor(0.4)).toBe('#d9822b');
  expect(coefColor(0.2)).toBe('#e03131');
  expect(coefColor(null)).toBe('#888');
});

test('показывает биржи, пары, оборот и исполнение; без заявок исполнение — «—»', async () => {
  getVenueLiquidityStats.mockResolvedValue(dto);
  const { container } = render(<VenueLiquidityPanel />);
  await waitFor(() => expect(container.textContent).toMatch(/binance/));
  expect(container.textContent).toMatch(/BTC\/USDT/);
  expect(container.textContent).toMatch(/\$ 1\.26 млн/);
  expect(container.textContent).toMatch(/72 %/);
  expect(container.textContent).toMatch(/20 %/);   // kraken — тонкая биржа
  const rows = Array.from(container.querySelectorAll('tbody tr'));
  const ethBtc = rows.find((r) => /ETH\/BTC/.test(r.textContent));
  expect(ethBtc.textContent).toMatch(/—/);          // f нет: заявок не было
  expect(ethBtc.textContent).not.toMatch(/100 %/);
});

test('пустой ответ — сообщение, не ошибка', async () => {
  getVenueLiquidityStats.mockResolvedValue({ venues: [] });
  const { container } = render(<VenueLiquidityPanel />);
  await waitFor(() => expect(container.textContent).toMatch(/Измерений пока нет/));
});

test('ошибка загрузки — понятное сообщение', async () => {
  getVenueLiquidityStats.mockRejectedValue(new Error('boom'));
  const { container } = render(<VenueLiquidityPanel />);
  await waitFor(() => expect(container.textContent).toMatch(/Не удалось загрузить измерения ликвидности/));
});
