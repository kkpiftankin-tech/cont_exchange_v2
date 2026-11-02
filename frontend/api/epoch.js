'use strict';
// Метка сброса позиций (эпоха) для потребителей истории (ADR-090 п. 2 и 6, CR-F18-018, Э1).
// Сброс создаёт запись в ce_reset_epoch; любой расчёт и любой график по истории обязан знать, к какой эпохе относится каждый его
// интервал, и не смешивать интервалы до и после сброса. Здесь только чистые функции без ввода-вывода: чтение таблицы и запись метки
// — в server.js. Помощник resolveEpochWindow — единственное место, где окно запроса истории сопоставляется с метками: каждый маршрут
// истории вызывает его, а не фильтрует «после сброса» сам.
//
// Эпоха 0 виртуальная: «до первой метки». Эпоха N занимает полуоткрытый интервал [at_N, at_{N+1}); у текущей правой границы нет.

const HOUR_MS = 3600 * 1000;
const finite = (v) => v !== null && v !== undefined && v !== '' && Number.isFinite(Number(v));

const VIRTUAL_EPOCH = Object.freeze({ epoch: 0, at_ms: 0, kind: 'virtual', note: '', actor: '', boot_id: '' });

// Строки таблицы (epoch_id, at_ms, kind, note, actor, boot_id) → отсортированный по времени список эпох без дублей и с возрастающим
// временем. Непригодная строка (нет номера или времени) отбрасывается, а не превращается в нулевую эпоху. null → null и испорченный порядок
// → null: «метки недоступны» (таблицы нет, Postgres не ответил, данные испорчены) отличается от пустого списка «меток ещё не было».
function normalizeEpochs(rows) {
  if (rows === null || rows === undefined) return null;
  const out = [];
  const seen = new Set();
  for (const r of Array.isArray(rows) ? rows : []) {
    if (!r || !finite(r.epoch_id !== undefined ? r.epoch_id : r.epoch) || !finite(r.at_ms)) continue;
    const epoch = Math.trunc(Number(r.epoch_id !== undefined ? r.epoch_id : r.epoch));
    if (epoch <= 0 || seen.has(epoch)) continue;
    seen.add(epoch);
    out.push({
      epoch, at_ms: Math.trunc(Number(r.at_ms)),
      kind: String(r.kind || ''), note: String(r.note || ''), actor: String(r.actor || ''), boot_id: String(r.boot_id || ''),
    });
  }
  out.sort((a, b) => (a.at_ms - b.at_ms) || (a.epoch - b.epoch));
  // Время обязано строго возрастать вместе с номером (так гарантирует таблица). Нарушение — испорченные данные: метки объявляются недоступными (null),
  // а не обрезаются до префикса — иначе тихо потерялась бы текущая эпоха и граница окон откатилась бы назад.
  for (let i = 1; i < out.length; i++) {
    if (!(out[i].at_ms > out[i - 1].at_ms) || !(out[i].epoch > out[i - 1].epoch)) return null;
  }
  return out;
}

// Текущая эпоха; пока меток не было — виртуальная нулевая.
function currentEpoch(epochs) {
  return Array.isArray(epochs) && epochs.length ? epochs[epochs.length - 1] : VIRTUAL_EPOCH;
}

// Номер эпохи, которой принадлежит момент tsMs: наибольшая at_ms <= tsMs; раньше первой метки — 0.
function epochAt(epochs, tsMs) {
  if (!Array.isArray(epochs) || !finite(tsMs)) return 0;
  const t = Number(tsMs);
  let ans = 0;
  for (const e of epochs) { if (e.at_ms <= t) ans = e.epoch; else break; }
  return ans;
}

// Эпоха по номеру (включая виртуальную 0); нет такой — null.
function findEpoch(epochs, n) {
  if (Number(n) === 0) return VIRTUAL_EPOCH;
  return (Array.isArray(epochs) ? epochs : []).find((e) => e.epoch === Number(n)) || null;
}

// Границы эпох: [{ epoch, from_ms, to_ms }], у виртуальной from_ms = 0, у текущей to_ms = null (открыта).
function epochWindows(epochs) {
  const list = Array.isArray(epochs) ? epochs : [];
  const out = [{ epoch: 0, from_ms: 0, to_ms: list.length ? list[0].at_ms : null }];
  for (let i = 0; i < list.length; i++) out.push({ epoch: list[i].epoch, from_ms: list[i].at_ms, to_ms: i + 1 < list.length ? list[i + 1].at_ms : null });
  return out;
}

const pick = (e) => ({ epoch: e.epoch, at_ms: e.at_ms, kind: e.kind, note: e.note });

function parseBool(v) {
  if (v === true || v === 1) return true;
  const s = String(v === undefined || v === null ? '' : v).trim().toLowerCase();
  return s === '1' || s === 'true' || s === 'yes';
}

function fail(status, error, message) { return { ok: false, status, error, message }; }

// Окно запроса истории → границы выборки и сведения для ответа.
//   q            — параметры запроса как пришли: hours, from_ms, to_ms, epoch, allow_cross_reset (строки или числа; не заданный параметр
//                  — undefined). Список epochs: normalizeEpochs(...) или null, если метки недоступны.
//   opts         — defaultHours (окно без параметров), maxHours (потолок окна, он же потолок для epoch=N).
// Правила:
//   * по умолчанию окно усекается до начала текущей эпохи (truncated_by_reset), позже метки данных до сброса не видно;
//   * allow_cross_reset=true разрешает окну пересекать метки; ответ тогда обязан пометить каждую точку её эпохой (tagEpoch);
//   * epoch=N — вся эпоха N целиком (а не окно в часах): вместе с hours/from_ms/to_ms это неоднозначно (400), неизвестный номер — 404;
//   * метки недоступны (epochs === null) — прежнее поведение без усечения и признак supported:false: выдумывать границу нельзя.
function resolveEpochWindow(epochs, q, nowMs, opts) {
  const o = opts || {};
  const query = q || {};
  const now = Number(nowMs);
  const defaultHours = finite(o.defaultHours) ? Number(o.defaultHours) : 24;
  const maxHours = finite(o.maxHours) ? Number(o.maxHours) : 168;
  const allowCross = parseBool(query.allow_cross_reset);
  const given = (v) => v !== undefined && v !== null && String(v).trim() !== '';
  // epoch задан, но не целое неотрицательное число — ошибка запроса, а не молчаливое окно текущей эпохи.
  if (given(query.epoch) && !/^\d+$/.test(String(query.epoch).trim())) return fail(400, 'bad_epoch', 'epoch должен быть целым неотрицательным числом');
  const hasEpoch = given(query.epoch);
  // hours разбирается как parseInt (прежнее поведение маршрутов): «12abc» → 12, «0» и мусор → окно по умолчанию.
  const hoursParsed = parseInt(String(query.hours), 10);
  const hasHours = Number.isFinite(hoursParsed) && hoursParsed >= 1;
  // from_ms/to_ms: конечные положительные числа в пределах диапазона даты JS, иначе 400 (а не 502 из-за to_timestamp вне диапазона).
  const msOk = (v) => finite(v) && Number(v) > 0 && Number(v) <= 8.64e15;
  if ((given(query.from_ms) && !msOk(query.from_ms)) || (given(query.to_ms) && !msOk(query.to_ms))) return fail(400, 'bad_window', 'from_ms и to_ms — положительные миллисекунды');
  const hasFrom = given(query.from_ms), hasTo = given(query.to_ms);
  const supported = Array.isArray(epochs);
  const list = supported ? epochs : [];
  const cur = currentEpoch(list);

  let reqFrom, to, epochRequested = null, clamped = false;
  if (hasEpoch) {
    if (hasHours || hasFrom || hasTo) return fail(400, 'ambiguous_window', 'epoch нельзя сочетать с hours, from_ms и to_ms');
    if (!supported) return fail(409, 'epochs_unavailable', 'метки сброса недоступны');
    const n = Math.trunc(Number(query.epoch));
    const e = findEpoch(list, n);
    if (!e) return fail(404, 'epoch_not_found', `эпохи ${n} нет`);
    epochRequested = n;
    const next = epochWindows(list).find((w) => w.epoch === n);
    reqFrom = e.at_ms;
    to = next && next.to_ms !== null ? next.to_ms : Math.max(now, e.at_ms + 1);   // метка может опережать часы BFF на миллисекунды: окно не бывает пустым
    // Очень старая эпоха не должна давать запрос за весь срок хранения: окно ограничено потолком (ближе к концу эпохи).
    if (to - reqFrom > maxHours * HOUR_MS) { reqFrom = to - maxHours * HOUR_MS; clamped = true; }
  } else {
    to = hasTo ? Number(query.to_ms) : now;
    if (hasFrom) reqFrom = Number(query.from_ms);
    else {
      const h = hasHours ? Math.max(1, Math.min(maxHours, hoursParsed)) : defaultHours;
      reqFrom = to - h * HOUR_MS;
    }
    if (!(to > reqFrom)) return fail(400, 'bad_window', 'to_ms должен быть больше from_ms');
    // Явные границы тоже ограничены потолком окна (иначе запрос на весь срок хранения с разрешением пересечения метки был бы тяжёлым).
    if (to - reqFrom > maxHours * HOUR_MS) return fail(400, 'window_too_large', `окно не может быть длиннее ${maxHours} ч`);
  }

  // Метки, оказавшиеся строго внутри запрошенного окна: именно они делят его на интервалы разных эпох.
  const inside = list.filter((e) => e.at_ms > reqFrom && e.at_ms < to);
  const crosses = inside.length > 0;
  let effFrom = reqFrom;
  let truncated = false;
  if (supported && !hasEpoch && !allowCross && crosses) {
    effFrom = inside[inside.length - 1].at_ms;   // начало эпохи, в которой лежит конец окна (при to = сейчас — текущей)
    truncated = true;
  }
  const windows = epochWindows(list).filter((w) => (w.to_ms === null || w.to_ms > effFrom) && w.from_ms < to);
  // Метки недоступны — какой эпохе принадлежат данные, неизвестно: пустой список, а не «эпоха 0».
  const windowEpochs = supported ? windows.map((w) => { const e = findEpoch(list, w.epoch); return e ? pick(e) : pick(VIRTUAL_EPOCH); }) : [];
  const markers = supported ? list.filter((e) => e.at_ms >= effFrom && e.at_ms <= to).map((e) => e.at_ms) : [];

  return {
    ok: true,
    supported,
    epoch: supported ? pick(cur) : null,
    window: {
      requested_from_ms: reqFrom, effective_from_ms: effFrom, to_ms: to,
      window_epochs: windowEpochs, crosses_reset: crosses, truncated_by_reset: truncated, allow_cross_reset: allowCross,
      epoch_requested: epochRequested, clamped_by_max_window: clamped, explicit_window: hasFrom || hasTo,
      // Длина запрошенного окна в часах (для window_hours ответа и размера интервалов): единый расчёт вместо повторного разбора hours в маршрутах.
      hours: Math.max(1, Math.min(maxHours, Math.ceil((to - reqFrom) / HOUR_MS))),
    },
    reset_markers: markers,
  };
}

// Блок для ответа любого маршрута истории. resolved — результат resolveEpochWindow (ok:true).
function epochBlock(resolved) {
  return {
    epoch_supported: resolved.supported,
    epoch: resolved.epoch,
    window: resolved.window,
    reset_markers: resolved.reset_markers,
  };
}

// Короткий блок для шапки ответов «текущего состояния» (позиции, заработок): только текущая эпоха.
function epochHeader(epochs) {
  if (!Array.isArray(epochs)) return { epoch_supported: false, epoch: null };
  return { epoch_supported: true, epoch: pick(currentEpoch(epochs)) };
}

// Строки с полем времени (мс) → те же строки с полем epoch (для allow_cross_reset: каждая точка несёт свою эпоху). Если у строки уже есть
// номер эпохи, записанный при записи (колонка epoch_id), он важнее определения по времени.
function tagEpoch(rows, epochs, tsField) {
  const f = tsField || 'ts_ms';
  return (Array.isArray(rows) ? rows : []).map((r) => {
    const own = r && finite(r.epoch_id) ? Math.trunc(Number(r.epoch_id)) : null;
    return { ...r, epoch: own !== null ? own : epochAt(epochs, r && r[f]) };
  });
}

module.exports = {
  VIRTUAL_EPOCH, normalizeEpochs, currentEpoch, epochAt, findEpoch, epochWindows,
  resolveEpochWindow, epochBlock, epochHeader, tagEpoch, parseBool,
};
