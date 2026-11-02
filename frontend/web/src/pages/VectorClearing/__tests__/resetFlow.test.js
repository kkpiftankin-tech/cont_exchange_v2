import { newRequestId, resetErrorInfo, resetSuccessText, RESET_PROMPT, RESET_TOOLTIP } from '../resetFlow';

// Поток кнопки сброса позиций (ADR-090): тексты и идентификатор запроса. Домена здесь нет — только показ кодов, которые вернул BFF.
describe('newRequestId', () => {
  test('использует crypto.randomUUID, если он есть', () => {
    expect(newRequestId({ randomUUID: () => '123e4567-e89b-12d3-a456-426614174000' }, 1, Math.random)).toBe('123e4567-e89b-12d3-a456-426614174000');
  });
  test('без crypto — 8–80 знаков [A-Za-z0-9_-] (ограничения BFF)', () => {
    const id = newRequestId(null, 1800000000000, () => 0.123456789);
    expect(id).toMatch(/^[A-Za-z0-9_-]{8,80}$/);
  });
  test('два вызова с разным временем дают разные идентификаторы', () => {
    expect(newRequestId(null, 1, () => 0.5)).not.toBe(newRequestId(null, 2, () => 0.5));
  });
});

describe('resetErrorInfo', () => {
  const err = (code, extra) => ({ response: { data: { error: code, ...extra } }, message: 'Request failed' });
  test('известные коды BFF → понятный текст, без повтора', () => {
    ['reset_not_allowed', 'epoch_changed', 'reset_too_frequent', 'reset_in_progress', 'note_required', 'ledger_unavailable'].forEach((c) => {
      const r = resetErrorInfo(err(c));
      expect(r.retry).toBe(false);
      expect(r.text).toMatch(/^сброс не выполнен: /);
    });
  });
  test('epoch_not_recorded: позиции уже сброшены, нужен повтор того же запроса', () => {
    const r = resetErrorInfo(err('epoch_not_recorded', { cleared: 68 }));
    expect(r.retry).toBe(true);
    expect(r.text).toMatch(/68/);
    expect(r.text).toMatch(/ещё раз/);
  });
  test('неизвестный код или сетевая ошибка → общий текст с сообщением', () => {
    expect(resetErrorInfo(err('weird')).text).toMatch(/ошибка сброса: Request failed/);
    expect(resetErrorInfo(new Error('Network Error')).text).toMatch(/Network Error/);
    expect(resetErrorInfo(null).text).toMatch(/ошибка сброса/);
  });
});

describe('resetSuccessText', () => {
  test('эпоха, название и предупреждение о том, что остатки площадок не сброшены (мост)', () => {
    const t = resetSuccessText({ cleared: 68, epoch: 4, note: 'опыт D', absolute: false });
    expect(t).toMatch(/сброшено агентов: 68/);
    expect(t).toMatch(/эпоха 4 «опыт D»/);
    expect(t).toMatch(/НЕ сброшены/);
  });
  test('повтор прежнего запроса не утверждает число сброшенных', () => {
    const t = resetSuccessText({ cleared: null, epoch: 4, note: 'опыт D', absolute: false, replay: true });
    expect(t).toMatch(/повтор прежнего запроса/);
    expect(t).not.toMatch(/сброшено агентов/);
  });
  test('абсолютная эпоха без предупреждения', () => {
    expect(resetSuccessText({ cleared: 1, epoch: 5, note: 'x', absolute: true })).not.toMatch(/НЕ сброшены/);
  });
});

describe('подсказки', () => {
  test('называют абсолютный сброс и скрипт', () => {
    expect(RESET_PROMPT).toMatch(/absolute_reset\.sh/);
    expect(RESET_TOOLTIP).toMatch(/absolute_reset\.sh/);
  });
});
