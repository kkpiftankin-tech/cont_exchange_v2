// Поток кнопки «Сбросить позиции» (ADR-090, CR-F18-018, Э1): тексты ответов и отказов, идентификатор запроса, подсказки.
// Чистые функции без запросов и без доменных расчётов (frontend-no-domain-compute): всё, что касается метки сброса, решает BFF; здесь только
// показ готовых кодов ответа.

// Идентификатор запроса: 8–80 знаков [A-Za-z0-9_-] (BFF отклоняет остальные). crypto.randomUUID, иначе время + случайные знаки.
export function newRequestId(cryptoLike, nowMs, rnd) {
  if (cryptoLike && typeof cryptoLike.randomUUID === 'function') return String(cryptoLike.randomUUID());
  const r = typeof rnd === 'function' ? rnd : Math.random;
  return `r${Number(nowMs).toString(36)}${r().toString(36).slice(2, 12)}${r().toString(36).slice(2, 8)}`.replace(/[^A-Za-z0-9_-]/g, '').slice(0, 80).padEnd(8, '0');
}

export const RESET_PROMPT = 'Название эксперимента (1–200 знаков). Сброс обнулит позиции, книги и хеджи агентов в ledger и создаст метку (эпоху): '
  + 'графики и разборы будут отделять данные до и после неё.\n\nВнимание: этот сброс НЕ обнуляет остатки площадок, план по активу и очередь переводов. '
  + 'Для начала эксперимента с исходного состояния используйте tools/reset/absolute_reset.sh.';

export const RESET_TOOLTIP = 'Сброс позиций агентов в ledger с меткой эпохи (название эксперимента обязательно). '
  + 'Остатки площадок не сбрасываются — абсолютный сброс: tools/reset/absolute_reset.sh';

const ERRORS = {
  reset_not_allowed: 'сброс запрещён на этом стенде (разрешён только в песочнице)',
  epoch_changed: 'эпоха уже сменилась (повторное нажатие или устаревший экран) — обновите страницу',
  reset_too_frequent: 'сброс возможен не чаще раза в минуту',
  reset_in_progress: 'другой сброс уже выполняется',
  request_id_conflict: 'этот запрос уже использован для другого сброса — повторите заново',
  note_required: 'нужно название эксперимента (1–200 знаков, без управляющих символов)',
  confirm_required: 'сброс не подтверждён',
  expected_epoch_required: 'не удалось определить текущую эпоху — обновите страницу',
  bad_request_id: 'некорректный идентификатор запроса — повторите',
  epoch_schema_unavailable: 'метки сброса недоступны — сброс не выполнен',
  postgres_unavailable: 'Postgres недоступен — сброс не выполнен',
  ledger_unavailable: 'ledger недоступен — сброс не выполнен',
  grpc_error: 'ledger не выполнил сброс — метка не создана',
  epoch_clock_skew: 'последняя метка опережает часы базы — сброс не выполнен',
  selective_reset_not_supported: 'выборочный сброс не поддерживается',
  unsupported_media_type: 'запрос отклонён (тип содержимого)',
  bad_origin: 'запрос отклонён (источник)',
};

// Сообщение об ошибке по ответу axios (e.response.data.error — код BFF). epoch_not_recorded — особый случай: позиции УЖЕ сброшены, нужно повторить
// тот же запрос (с тем же request_id), чтобы дописалась метка; признак возвращается в retry.
export function resetErrorInfo(err) {
  const d = err && err.response && err.response.data;
  const code = d && d.error;
  if (code === 'epoch_not_recorded') {
    return { retry: true, text: `позиции в ledger сброшены (${(d && d.cleared) || 0}), но метка не записана — нажмите кнопку ещё раз: тот же запрос допишет метку` };
  }
  if (code && ERRORS[code]) return { retry: false, text: `сброс не выполнен: ${ERRORS[code]}` };
  return { retry: false, text: 'ошибка сброса: ' + ((err && err.message) || 'не выполнено') };
}

export function resetSuccessText(data) {
  const d = data || {};
  const n = d.cleared === null || d.cleared === undefined ? '' : `сброшено агентов: ${d.cleared} ✓ — `;
  const replay = d.replay ? 'повтор прежнего запроса: ' : '';
  return `${replay}${n}эпоха ${d.epoch} «${d.note}», позиции копятся с нуля`
    + (d.absolute === false ? '. Остатки площадок и план по активу НЕ сброшены (для исходного состояния — tools/reset/absolute_reset.sh)' : '');
}
