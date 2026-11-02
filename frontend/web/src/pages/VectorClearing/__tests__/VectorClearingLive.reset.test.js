import React from 'react';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import VectorClearingLive from '../VectorClearingLive';

// Кнопка «Сбросить позиции» (ADR-090, CR-F18-018): название эксперимента обязательно, запрос несёт подтверждение, request_id и номер эпохи,
// который видит экран (expected_epoch); ответы BFF показываются понятными сообщениями; после «позиции сброшены, метка не записана» повтор
// использует тот же request_id. Домена на экране нет — только показ кодов, которые вернул BFF.
jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));
const axios = require('axios').default;
jest.mock('react-router-dom', () => ({ useNavigate: () => () => {} }), { virtual: true });
jest.mock('../../../api/authService', () => ({ isAuthenticated: () => Promise.resolve(true) }));
jest.mock('../../../components/NavBar', () => () => null);

let epochs;   // что отвечает GET /vector-clearing/epochs
beforeEach(() => {
  axios.get.mockReset(); axios.post.mockReset();
  epochs = { data: { supported: true, current: { epoch: 3, at_ms: 1, kind: 'absolute', note: 'опыт C' }, epochs: [] } };
  axios.get.mockImplementation((url) => {
    if (/vector-clearing\/epochs$/.test(url)) return Promise.resolve(epochs);
    return Promise.resolve({ data: [] });
  });
  window.prompt = jest.fn(() => 'опыт D');
});

const resetPosts = () => axios.post.mock.calls.filter((c) => /reset-positions$/.test(c[0]));
const clickReset = async () => {
  const btn = await screen.findByText('⟲ Сбросить позиции');
  fireEvent.click(btn);
};

test('запрос несёт название, подтверждение, request_id и номер эпохи, который видит экран; в сообщении эпоха и предупреждение', async () => {
  axios.post.mockResolvedValue({ data: { cleared: 68, epoch: 4, note: 'опыт D', absolute: false, kind: 'bridge' } });
  render(<VectorClearingLive />);
  await clickReset();
  await waitFor(() => expect(resetPosts().length).toBe(1));
  const body = resetPosts()[0][1];
  expect(body.note).toBe('опыт D');
  expect(body.confirm).toBe(true);
  expect(body.expected_epoch).toBe(3);
  expect(body.request_id).toMatch(/^[A-Za-z0-9_-]{8,80}$/);
  await waitFor(() => expect(screen.getByText(/эпоха 4 «опыт D»/)).toBeTruthy());
  expect(screen.getByText(/НЕ сброшены/)).toBeTruthy();
  expect(window.prompt.mock.calls[0][0]).toMatch(/absolute_reset\.sh/);
});

test('отмена вопроса или пустое название — запрос не отправляется', async () => {
  render(<VectorClearingLive />);
  window.prompt = jest.fn(() => null);
  await clickReset();
  await waitFor(() => expect(window.prompt).toHaveBeenCalled());
  expect(resetPosts().length).toBe(0);
  window.prompt = jest.fn(() => '   ');
  fireEvent.click(screen.getByText('⟲ Сбросить позиции'));
  await waitFor(() => expect(screen.getByText(/нужно название эксперимента/)).toBeTruthy());
  expect(resetPosts().length).toBe(0);
});

test('не удалось получить текущую эпоху — сброс не выполняется и вопрос не задаётся', async () => {
  axios.get.mockImplementation((url) => (/vector-clearing\/epochs$/.test(url) ? Promise.reject(new Error('down')) : Promise.resolve({ data: [] })));
  render(<VectorClearingLive />);
  await clickReset();
  await waitFor(() => expect(screen.getByText(/не удалось получить текущую эпоху/)).toBeTruthy());
  expect(window.prompt).not.toHaveBeenCalled();
  expect(resetPosts().length).toBe(0);
});

test('метки недоступны (supported:false) — expected_epoch равен 0', async () => {
  epochs = { data: { supported: false, current: null, epochs: [] } };
  axios.post.mockResolvedValue({ data: { cleared: 1, epoch: 1, note: 'опыт D', absolute: false } });
  render(<VectorClearingLive />);
  await clickReset();
  await waitFor(() => expect(resetPosts().length).toBe(1));
  expect(resetPosts()[0][1].expected_epoch).toBe(0);
});

test('отказ BFF показан понятным текстом (эпоха сменилась)', async () => {
  axios.post.mockRejectedValue({ response: { data: { error: 'epoch_changed' } }, message: 'Request failed' });
  render(<VectorClearingLive />);
  await clickReset();
  await waitFor(() => expect(screen.getByText(/сброс не выполнен: эпоха уже сменилась/)).toBeTruthy());
});

test('«позиции сброшены, метка не записана»: повтор берёт тот же request_id и не задаёт вопрос заново', async () => {
  axios.post.mockRejectedValueOnce({ response: { data: { error: 'epoch_not_recorded', cleared: 68 } }, message: 'x' });
  axios.post.mockResolvedValueOnce({ data: { cleared: null, epoch: 4, note: 'опыт D', absolute: false, replay: false } });
  render(<VectorClearingLive />);
  await clickReset();
  await waitFor(() => expect(screen.getByText(/метка не записана — нажмите кнопку ещё раз/)).toBeTruthy());
  const first = resetPosts()[0][1];
  window.prompt.mockClear();
  fireEvent.click(screen.getByText('⟲ Сбросить позиции'));
  await waitFor(() => expect(resetPosts().length).toBe(2));
  expect(window.prompt).not.toHaveBeenCalled();
  const second = resetPosts()[1][1];
  expect(second.request_id).toBe(first.request_id);
  expect(second.note).toBe(first.note);
  expect(second.expected_epoch).toBe(first.expected_epoch);
});
