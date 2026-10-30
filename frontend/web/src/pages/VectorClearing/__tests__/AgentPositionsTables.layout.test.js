import React from 'react';
import { render } from '@testing-library/react';
import { AgentPositionsTables } from '../VectorClearingLive';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));
jest.mock('react-router-dom', () => ({ useNavigate: () => () => {} }), { virtual: true });
jest.mock('../../../api/authService', () => ({ isAuthenticated: () => Promise.resolve(true) }));
jest.mock('../../../components/NavBar', () => () => null);

// Раскладка панели «Живые позиции»: слева внутренняя биржа CE, справа внешние биржи; заявка и сделки — в строке своей валюты;
// база и квота сделок — в одном столбце; R и U объяснены на экране.
const translator = (over = {}) => ({
  agent_id: 'T_BTC_binance', agent_kind: 'translator', base: 'BTC', quote: 'USDT', venue: 'binance', hasDelta: true,
  pos_base_before: 0.5, delta_base: -0.01, pos_base: 0.49, pos_quote_before: -1200, delta_quote: 24, pos_quote: -1176,
  realized_usdt: 3.2, unrealized_usdt: 1.1, pnl_usdt: 4.3, trade_fills: 12,
  traded_base: -0.25, traded_quote: 15000,
  hedge_symbol: 'BTC/USDT', hedge_side: 'SELL', hedge_venue: 'binance', hedge_sent: 0.0123, hedge_filled: 0.01, hedge_status: 'PARTIALLY_FILLED',
  ...over,
});
const arbitrageur = (over = {}) => ({
  agent_id: 'A_ETH_binanceokx', agent_kind: 'arbitrageur', base: 'ETH', quote: 'USDT', venue: '', hasDelta: true,
  arb: { src: 'binance', dst: 'okx', unit: 'ETH', leg_src: 1.7579, leg_dst: -1.7579, bleg_src: 1.7, bleg_dst: -1.7, dleg_src: 0.0579, dleg_dst: -0.0579 },
  realized_usdt: 2, unrealized_usdt: -1, pnl_usdt: 1, trade_fills: 5,
  hedge_by_venue: {
    binance: { symbol: 'ETH/USDT', side: 'SELL', sent: 0.5, filled: 0.5, status: 'FILLED', ms: 1 },
    okx: { symbol: 'ETH/USDT', side: 'BUY', sent: 0.4, filled: 0, status: 'EXPIRED', ms: 2 },
  },
  trade_by_venue: {
    binance: { traded_base: -0.5, traded_quote: 1000, trade_fills: 3 },
    okx: { traded_base: 0.2, traded_quote: -401, trade_fills: 2 },
  },
  ...over,
});

const legRows = (container) => Array.from(container.querySelectorAll('tbody tr')).filter((tr) => !tr.classList.contains('vc-group-head'));
const text = (el) => (el ? el.textContent.replace(/\s+/g, ' ').trim() : null);

test('шапка делит колонки на внутреннюю биржу CE и внешнюю', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator()]} title="t" />);
  const gi = container.querySelector('[data-testid="grp-internal"]');
  const ge = container.querySelector('[data-testid="grp-external"]');
  expect(gi).not.toBeNull();
  expect(ge).not.toBeNull();
  expect(gi.getAttribute('colspan')).toBe('4');
  expect(ge.getAttribute('colspan')).toBe('4');
  expect(text(gi)).toMatch(/Внутренняя биржа CE/);
  expect(text(ge)).toMatch(/Внешняя биржа/);
  const heads = Array.from(container.querySelectorAll('thead th')).map(text);
  expect(heads.some((h) => h === 'отправлено')).toBe(true);
  expect(heads.some((h) => h === 'сделки')).toBe(true);
  expect(heads.some((h) => /сделки: (база|квота)/.test(h))).toBe(false);   // база и квота слиты в один столбец
});

test('переводчик: «отправлено/исполнено» стоят в строке валюты заявки (BTC), в строке USDT пусто', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator()]} title="t" />);
  const [btc, usdt] = legRows(container);
  expect(text(btc.querySelector('[data-testid="hedge-sent"]'))).toBe('0.012300 BTC');
  expect(text(btc.querySelector('[data-testid="hedge-filled"]'))).toBe('0.010000 BTC');
  expect(text(usdt.querySelector('[data-testid="hedge-sent"]'))).toBe('');
  expect(text(usdt.querySelector('[data-testid="hedge-filled"]'))).toBe('');
});

test('переводчик с кросс-парой ETH/BTC: заявка по ETH/BTC — в строке ETH; заявка по BTC/USDT у ETH_BTC-агента — в строке BTC', () => {
  const cross = translator({ agent_id: 'T_ETH_BTC_binance', base: 'ETH', quote: 'BTC', hedge_symbol: 'ETH/BTC' });
  const { container, rerender } = render(<AgentPositionsTables agentRows={[cross]} title="t" />);
  let [r0, r1] = legRows(container);
  expect(text(r0.querySelector('[data-testid="hedge-sent"]'))).toBe('0.012300 ETH');
  expect(text(r1.querySelector('[data-testid="hedge-sent"]'))).toBe('');
  rerender(<AgentPositionsTables agentRows={[{ ...cross, hedge_symbol: 'BTC/USDT' }]} title="t" />);
  [r0, r1] = legRows(container);
  expect(text(r0.querySelector('[data-testid="hedge-sent"]'))).toBe('');
  expect(text(r1.querySelector('[data-testid="hedge-sent"]'))).toBe('0.012300 BTC');
});

test('агент без заявок: прочерк в первой строке, во второй пусто', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator({ hedge_status: null })]} title="t" />);
  const [r0, r1] = legRows(container);
  expect(text(r0.querySelector('[data-testid="hedge-sent"]'))).toBe('—');
  expect(text(r1.querySelector('[data-testid="hedge-sent"]'))).toBe('');
});

test('сделки: база в строке базовой валюты, квота в строке котируемой — в одном столбце', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator()]} title="t" />);
  const [btc, usdt] = legRows(container);
  expect(text(btc.querySelector('[data-testid="leg-trades"]'))).toBe('-0.250000 BTC');
  expect(text(usdt.querySelector('[data-testid="leg-trades"]'))).toBe('+15000.00 USDT');
});

test('арбитражёр: заявка и сделки по бирже каждой ноги (база и квота друг под другом)', () => {
  const { container } = render(<AgentPositionsTables agentRows={[arbitrageur()]} title="t" />);
  const [src, dst] = legRows(container);
  expect(text(src.querySelector('[data-testid="hedge-sent"]'))).toBe('0.500000 ETH');
  expect(text(dst.querySelector('[data-testid="hedge-sent"]'))).toBe('0.400000 ETH');
  expect(text(dst.querySelector('[data-testid="hedge-filled"]'))).toBe('0 ETH');
  expect(src.querySelector('[data-testid="leg-trades"]').children.length).toBe(2);
  expect(text(src.querySelector('[data-testid="leg-trades"]'))).toBe('-0.500000 ETH+1000.00 USDT');
  expect(text(dst.querySelector('[data-testid="leg-trades"]'))).toBe('+0.200000 ETH-401.00 USDT');
});

test('арбитражёр: на бирже ноги нет заявки/сделок → прочерк', () => {
  const a = arbitrageur({ hedge_by_venue: { binance: { symbol: 'ETH/USDT', side: 'SELL', sent: 1, filled: 1, status: 'FILLED', ms: 1 } }, trade_by_venue: {} });
  const { container } = render(<AgentPositionsTables agentRows={[a]} title="t" />);
  const [src, dst] = legRows(container);
  expect(text(src.querySelector('[data-testid="hedge-sent"]'))).toBe('1.0000 ETH');
  expect(text(dst.querySelector('[data-testid="hedge-sent"]'))).toBe('—');
  expect(text(dst.querySelector('[data-testid="leg-trades"]'))).toBe('—');
});

test('строка группы растянута на все 10 колонок', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator()]} title="t" />);
  expect(container.querySelector('.vc-group-head td').getAttribute('colspan')).toBe('10');
});

test('R и U объяснены на экране: определения, «без комиссий», подпись в шапке', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator()]} title="t" />);
  const legend = text(container.querySelector('[data-testid="ru-explain"]'));
  expect(legend).toMatch(/R — реализованный заработок/);
  expect(legend).toMatch(/U — нереализованный результат/);
  expect(legend).toMatch(/без комиссий/);
  expect(text(container.querySelector('[data-testid="ru-head"]'))).toMatch(/R реализ\. · U нереализ\./);
});

test('клик по строке агента вызывает выбор (откроется график заработка)', () => {
  const onSelect = jest.fn();
  const { container } = render(<AgentPositionsTables agentRows={[translator()]} title="t" onSelectAgent={onSelect} />);
  legRows(container)[0].click();
  expect(onSelect).toHaveBeenCalledWith('T_BTC_binance');
});

test('символ заявки без разделителя (BTCUSDT, как в графе) тоже попадает в строку своей валюты', () => {
  const cross = translator({ agent_id: 'T_ETH_BTC_binance', base: 'ETH', quote: 'BTC', hedge_symbol: 'ETHBTC' });
  const { container, rerender } = render(<AgentPositionsTables agentRows={[cross]} title="t" />);
  let [r0, r1] = legRows(container);
  expect(text(r0.querySelector('[data-testid="hedge-sent"]'))).toBe('0.012300 ETH');
  expect(text(r1.querySelector('[data-testid="hedge-sent"]'))).toBe('');
  rerender(<AgentPositionsTables agentRows={[{ ...cross, hedge_symbol: 'BTCUSDT' }]} title="t" />);
  [r0, r1] = legRows(container);
  expect(text(r0.querySelector('[data-testid="hedge-sent"]'))).toBe('');
  expect(text(r1.querySelector('[data-testid="hedge-sent"]'))).toBe('0.012300 BTC');
});

test('выгода по теории: колонка во внутренней группе, значение, подсказка с долей учтённых тактов, итог по видам', () => {
  const rows = [translator({ surplus_usdt: 12.5, surplus_coverage: 0.97 }), arbitrageur({ surplus_usdt: 1234.567, surplus_coverage: 0.9 })];
  const { container } = render(<AgentPositionsTables agentRows={rows} title="t" />);
  const head = container.querySelector('[data-testid="surplus-head"]');
  expect(head).not.toBeNull();
  expect(text(head)).toMatch(/выгода по теории/);
  const cells = Array.from(container.querySelectorAll('[data-testid="surplus-cell"]'));
  expect(cells.length).toBe(2);
  expect(text(cells[0])).toBe('12,50');
  expect(text(cells[1])).toBe('1 234,57');
  expect(cells[0].querySelector('span').getAttribute('title')).toMatch(/Учтено 97 % тактов/);
  expect(cells[0].getAttribute('rowspan')).toBe('2');
  const tot = text(container.querySelector('[data-testid="surplus-totals"]'));
  expect(tot).toMatch(/переводчики 12,50/);
  expect(tot).toMatch(/арбитражёры 1 234,57/);
});

test('выгода по теории: нет данных — прочерк, итога нет; у связки без внешних сделок блок итогов всё равно показан', () => {
  const noData = render(<AgentPositionsTables agentRows={[translator({ realized_usdt: null, unrealized_usdt: null, pnl_usdt: null })]} title="t" />);
  expect(text(noData.container.querySelector('[data-testid="surplus-cell"]'))).toBe('—');
  expect(noData.container.querySelector('[data-testid="surplus-totals"]')).toBeNull();
  const arbOnly = render(<AgentPositionsTables agentRows={[arbitrageur({ surplus_usdt: 5, surplus_coverage: 1 })]} title="t" />);
  expect(arbOnly.container.querySelector('[data-testid="surplus-totals"]')).not.toBeNull();
});
