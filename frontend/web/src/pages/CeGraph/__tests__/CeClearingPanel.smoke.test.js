import React from 'react';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import CeClearingPanel from '../CeClearingPanel';
import fixture from './ceClearingView.fixture.json';   // вид BFF (buildClearingView) на реальных JSON-снимках matching

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn() } }));
const axios = require('axios').default;

test('потока нет: пояснение словами, циклы заблокированы, потенциалы и ликвидность бирж', async () => {
  axios.get.mockResolvedValue({ data: fixture.blocked });
  const { container } = render(<CeClearingPanel />);
  await waitFor(() => expect(container.querySelector('.cp-headline')).not.toBeNull());
  expect(axios.get.mock.calls[0][0]).toMatch(/vector-clearing\/ce-clearing$/);
  // главный вывод: потока нет, рёбра внутри полок
  expect(container.querySelector('.cp-headline').textContent).toMatch(/Потока нет/);
  expect(container.querySelector('.cp-headline').textContent).toMatch(/внутри своих полок/);
  // пояснения: заблокированный арбитраж, состав порога, отброшенные котировки
  const bullets = [...container.querySelectorAll('.cp-explain li')].map((n) => n.textContent).join(' | ');
  expect(bullets).toMatch(/арбитраж заблокирован/);
  expect(bullets).toMatch(/полка по возрасту данных ADR-075/);
  expect(bullets).toMatch(/не попали котировки/);
  // чипы: решатель сошёлся, активных рёбер 0
  expect(container.querySelector('.cp-chips').textContent).toMatch(/решатель сошёлся/);
  expect(container.querySelector('.cp-chips').textContent).toMatch(/рёбер активно\s*0\s*из/);
  // таблица циклов: все закрыты, показан порог и «не хватает»
  const cycRows = [...container.querySelectorAll('table.cp-tbl')][0].querySelectorAll('tbody tr');
  expect(cycRows.length).toBeGreaterThan(0);
  expect([...cycRows].every((r) => /закрыт/.test(r.textContent))).toBe(true);
  // таблица рёбер: есть переводчики и арбитражёры, состояние «внутри полки»
  expect(container.textContent).toMatch(/переводчик/);
  expect(container.textContent).toMatch(/арбитражёр/);
  expect(container.textContent).toMatch(/внутри полки/);
  // потенциалы узлов
  expect(container.textContent).toMatch(/Потенциалы узлов/);
  expect(container.textContent).toMatch(/⚓/);                                  // закреплённый узел-нумерарий
  // ликвидность бирж: биржа, возраст опроса, отброшенная котировка
  expect(container.textContent).toMatch(/Ликвидность внешних бирж/);
  expect(container.textContent).toMatch(/binance/);
  expect(container.textContent).toMatch(/отброшена: устарела/);
});

test('поток есть: зелёный вывод, открытые циклы', async () => {
  axios.get.mockResolvedValue({ data: fixture.open });
  const { container } = render(<CeClearingPanel />);
  await waitFor(() => expect(container.querySelector('.cp-headline')).not.toBeNull());
  expect(container.querySelector('.cp-headline').textContent).toMatch(/Поток есть/);
  expect(container.querySelector('.cp-explain.cp-ok')).not.toBeNull();
  expect(container.querySelectorAll('.cp-row-on').length).toBeGreaterThan(0);   // подсвечены активные рёбра / открытые циклы
  expect(container.textContent).toMatch(/есть поток/);
  expect(container.textContent).toMatch(/открыт/);
});

test('нет снимка от matching: честное сообщение, таблицы решения не выводятся нулями', async () => {
  axios.get.mockResolvedValue({ data: fixture.none });
  const { container } = render(<CeClearingPanel />);
  await waitFor(() => expect(container.querySelector('.cp-headline')).not.toBeNull());
  expect(container.querySelector('.cp-headline').textContent).toMatch(/Клиринг не прислал решение/);
  expect(container.querySelector('.cp-explain.cp-err')).not.toBeNull();
  expect(container.textContent).toMatch(/Снимок решения клиринга недоступен/);
  expect(container.textContent).not.toMatch(/Потенциалы узлов/);
  expect(container.textContent).toMatch(/Ликвидность внешних бирж/);         // состояние бирж из venues показывается и без снимка
});

test('устаревший снимок: заголовок «решение устарело», а не «потока нет»', async () => {
  axios.get.mockResolvedValue({ data: fixture.stale });
  const { container } = render(<CeClearingPanel />);
  await waitFor(() => expect(container.querySelector('.cp-headline')).not.toBeNull());
  expect(container.querySelector('.cp-headline').textContent).toMatch(/Решение клиринга устарело/);
  expect(container.querySelector('.cp-explain.cp-err')).not.toBeNull();
  expect(container.querySelector('.cp-chips').textContent).toMatch(/решение\s*12[0-9][,.]?\d*\s*с назад/);
  expect(container.querySelector('.cp-chip-warn')).not.toBeNull();            // чип возраста подсвечен
});

test('ошибка загрузки BFF: сообщение, страница не падает', async () => {
  axios.get.mockRejectedValue(new Error('boom'));
  render(<CeClearingPanel />);
  await waitFor(() => expect(screen.getByText(/не удалось загрузить решение клиринга/)).toBeTruthy());
});

test('«показать все» раскрывает длинную таблицу рёбер', async () => {
  const base = fixture.blocked;
  const many = Array.from({ length: 20 }, (_, i) => ({ ...base.solve.edges[0], name: `T_X${i}_binance`, margin_pm: 1 + i }));
  axios.get.mockResolvedValue({ data: { ...base, solve: { ...base.solve, edges: many } } });
  const { container } = render(<CeClearingPanel />);
  await waitFor(() => expect(container.querySelector('.cp-headline')).not.toBeNull());
  const edgesTbl = () => [...container.querySelectorAll('table.cp-tbl')].find((t) => /Якорь/.test(t.textContent));
  expect(edgesTbl().querySelectorAll('tbody tr').length).toBe(12);
  fireEvent.click(screen.getByText(/показать все \(20\)/));
  expect(edgesTbl().querySelectorAll('tbody tr').length).toBe(20);
});
