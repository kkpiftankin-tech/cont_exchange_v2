// Self-contained node-тест: `node epoch.test.js` (у frontend/api нет jest).
const {
  VIRTUAL_EPOCH, normalizeEpochs, currentEpoch, epochAt, findEpoch, epochWindows, resolveEpochWindow, epochBlock, epochHeader, tagEpoch, parseBool,
} = require('./epoch');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}

const H = 3600 * 1000;
const NOW = 1_800_000_000_000;
// Три метки: 1 — 30 ч назад (унаследованная), 2 — 10 ч назад (мост), 3 — 2 ч назад (абсолютная, текущая).
const rows = [
  { epoch_id: '3', at_ms: String(NOW - 2 * H), kind: 'absolute', note: 'опыт C', actor: 'owner', boot_id: 'b3' },
  { epoch_id: 1, at_ms: NOW - 30 * H, kind: 'legacy', note: 'legacy', actor: 'migration' },
  { epoch_id: 2, at_ms: NOW - 10 * H, kind: 'bridge', note: 'опыт B', actor: 'owner' },
];
const E = normalizeEpochs(rows);

// --- normalizeEpochs
check('эпохи отсортированы по времени, номера приведены к числам', E.map((e) => e.epoch).join() === '1,2,3' && typeof E[2].at_ms === 'number');
check('нет данных → null (метки недоступны), пустой список → [] (меток не было)', normalizeEpochs(null) === null && normalizeEpochs([]).length === 0);
check('строка без номера или времени отбрасывается, а не становится нулевой эпохой', normalizeEpochs([{ epoch_id: 1 }, { at_ms: 5 }, { epoch_id: 0, at_ms: 5 }, { epoch_id: 2, at_ms: 7 }]).length === 1);
check('дубль номера отбрасывается', normalizeEpochs([{ epoch_id: 1, at_ms: 1 }, { epoch_id: 1, at_ms: 2 }]).length === 1);
check('нарушение порядка (время не растёт вместе с номером) → null: метки недоступны, текущая эпоха не теряется молча',
  normalizeEpochs([{ epoch_id: 1, at_ms: 10 }, { epoch_id: 2, at_ms: 15 }, { epoch_id: 3, at_ms: 12 }]) === null
  && normalizeEpochs([{ epoch_id: 1, at_ms: 100 }, { epoch_id: 2, at_ms: 100 }, { epoch_id: 3, at_ms: 200 }]) === null);

// --- currentEpoch / epochAt / findEpoch / epochWindows
check('текущая эпоха — последняя', currentEpoch(E).epoch === 3);
check('меток нет → виртуальная нулевая', currentEpoch([]) === VIRTUAL_EPOCH && currentEpoch(null).epoch === 0);
check('epochAt: до первой метки — 0', epochAt(E, NOW - 31 * H) === 0);
check('epochAt: ровно в момент метки — уже новая эпоха (интервал [at, next))', epochAt(E, NOW - 10 * H) === 2 && epochAt(E, NOW - 10 * H - 1) === 1);
check('epochAt: после последней метки — текущая', epochAt(E, NOW) === 3);
check('epochAt: нечисловое время → 0', epochAt(E, null) === 0 && epochAt(E, 'x') === 0);
check('findEpoch: 0 виртуальная, неизвестная — null', findEpoch(E, 0) === VIRTUAL_EPOCH && findEpoch(E, 9) === null && findEpoch(E, 2).kind === 'bridge');
const W = epochWindows(E);
check('окна: 0..первая метка, затем по меткам, у текущей правой границы нет', W.length === 4 && W[0].to_ms === E[0].at_ms && W[3].to_ms === null && W[1].to_ms === E[1].at_ms);

// --- resolveEpochWindow: по умолчанию окно усекается до текущей эпохи
let r = resolveEpochWindow(E, { hours: '24' }, NOW, { defaultHours: 24, maxHours: 168 });
check('24 ч пересекают метки 2 и 3 → усечение до начала текущей эпохи', r.ok && r.window.truncated_by_reset && r.window.effective_from_ms === NOW - 2 * H);
check('запрошенное начало сохраняется в ответе', r.window.requested_from_ms === NOW - 24 * H && r.window.to_ms === NOW);
check('crosses_reset = true, в окне только текущая эпоха', r.window.crosses_reset === true && r.window.window_epochs.length === 1 && r.window.window_epochs[0].epoch === 3);
check('маркер сброса на левой границе окна', r.reset_markers.length === 1 && r.reset_markers[0] === NOW - 2 * H);
check('в ответе текущая эпоха с названием эксперимента', r.epoch.epoch === 3 && r.epoch.note === 'опыт C' && r.supported === true);
r = resolveEpochWindow(E, { hours: '1' }, NOW, { defaultHours: 24 });
check('окно целиком внутри текущей эпохи не усекается', r.window.truncated_by_reset === false && r.window.crosses_reset === false && r.window.effective_from_ms === NOW - H);
r = resolveEpochWindow(E, {}, NOW, { defaultHours: 6 });
check('без параметров — окно по умолчанию (6 ч) пересекает текущую метку и усекается', r.window.requested_from_ms === NOW - 6 * H && r.window.truncated_by_reset === true && r.window.effective_from_ms === NOW - 2 * H);
r = resolveEpochWindow(E, { hours: '9999' }, NOW, { defaultHours: 24, maxHours: 168 });
check('hours ограничен потолком', r.window.requested_from_ms === NOW - 168 * H);

// --- allow_cross_reset
r = resolveEpochWindow(E, { hours: '24', allow_cross_reset: 'true' }, NOW, { defaultHours: 24 });
check('allow_cross_reset: окно не усекается, перечислены все затронутые эпохи', r.window.effective_from_ms === NOW - 24 * H && !r.window.truncated_by_reset
  && r.window.crosses_reset && r.window.window_epochs.map((e) => e.epoch).join() === '1,2,3');
check('allow_cross_reset: маркеры обеих меток внутри окна', r.reset_markers.join() === [NOW - 10 * H, NOW - 2 * H].join());
check('parseBool: 1/true/yes — да, остальное — нет', parseBool('1') && parseBool('TRUE') && parseBool(true) && !parseBool('0') && !parseBool(undefined) && !parseBool('no'));

// --- epoch=N
r = resolveEpochWindow(E, { epoch: '2' }, NOW, { maxHours: 168 });
check('epoch=2 — вся эпоха от метки до следующей', r.ok && r.window.effective_from_ms === NOW - 10 * H && r.window.to_ms === NOW - 2 * H && r.window.epoch_requested === 2);
check('epoch=2: окно целиком в одной эпохе, пересечения нет', r.window.crosses_reset === false && r.window.window_epochs.length === 1 && r.window.window_epochs[0].epoch === 2);
r = resolveEpochWindow(E, { epoch: '3' }, NOW, {});
check('epoch=текущая — от метки до сейчас', r.window.effective_from_ms === NOW - 2 * H && r.window.to_ms === NOW);
r = resolveEpochWindow(E, { epoch: '0' }, NOW, { maxHours: 168 });
check('epoch=0 (до первой метки) допустима, но ограничена потолком окна', r.ok && r.window.to_ms === NOW - 30 * H && r.window.clamped_by_max_window === true);
r = resolveEpochWindow(E, { epoch: '7' }, NOW, {});
check('неизвестная эпоха → 404 epoch_not_found', !r.ok && r.status === 404 && r.error === 'epoch_not_found');
r = resolveEpochWindow(E, { epoch: '2', hours: '5' }, NOW, {});
check('epoch вместе с hours → 400 ambiguous_window', !r.ok && r.status === 400 && r.error === 'ambiguous_window');
r = resolveEpochWindow(E, { epoch: '2', from_ms: String(NOW - H) }, NOW, {});
check('epoch вместе с from_ms → 400 ambiguous_window', !r.ok && r.error === 'ambiguous_window');
r = resolveEpochWindow(null, { epoch: '2' }, NOW, {});
check('epoch при недоступных метках → 409', !r.ok && r.status === 409);

// --- from_ms / to_ms
r = resolveEpochWindow(E, { from_ms: String(NOW - 20 * H), to_ms: String(NOW - 5 * H) }, NOW, {});
check('окно в прошлом, пересекающее метку 2, усекается до эпохи, где лежит его конец', r.window.crosses_reset && r.window.effective_from_ms === NOW - 10 * H && r.window.window_epochs[0].epoch === 2);
r = resolveEpochWindow(E, { from_ms: String(NOW - 9 * H), to_ms: String(NOW - 5 * H) }, NOW, {});
check('окно в прошлом целиком внутри эпохи 2 — как есть', !r.window.crosses_reset && !r.window.truncated_by_reset && r.window.window_epochs[0].epoch === 2);
r = resolveEpochWindow(E, { from_ms: String(NOW), to_ms: String(NOW - H) }, NOW, {});
check('to <= from → 400 bad_window', !r.ok && r.error === 'bad_window');

// --- метки недоступны / ещё не было
r = resolveEpochWindow(null, { hours: '24' }, NOW, { defaultHours: 24 });
check('метки недоступны: supported=false, epoch=null, усечения нет (прежнее поведение)', r.ok && r.supported === false && r.epoch === null
  && r.window.effective_from_ms === NOW - 24 * H && !r.window.truncated_by_reset && r.reset_markers.length === 0);
r = resolveEpochWindow([], { hours: '24' }, NOW, { defaultHours: 24 });
check('меток ещё не было: supported=true, эпоха 0, усечения нет', r.supported === true && r.epoch.epoch === 0 && !r.window.truncated_by_reset
  && r.window.window_epochs.length === 1 && r.window.window_epochs[0].epoch === 0);

// --- ADR-090, ревью: строгий разбор параметров и согласованные часы окна
r = resolveEpochWindow(E, { epoch: 'abc' }, NOW, {});
check('epoch=abc → 400 bad_epoch (а не молчаливое окно текущей эпохи)', !r.ok && r.status === 400 && r.error === 'bad_epoch');
r = resolveEpochWindow(E, { epoch: '-1' }, NOW, {});
check('epoch=-1 → 400 bad_epoch', !r.ok && r.error === 'bad_epoch');
r = resolveEpochWindow(E, { epoch: '' }, NOW, { defaultHours: 24 });
check('пустой epoch = не задан', r.ok && r.window.epoch_requested === null);
r = resolveEpochWindow(E, { from_ms: String(NOW - 500 * H), to_ms: String(NOW) }, NOW, { maxHours: 168 });
check('явное окно длиннее потолка → 400 window_too_large', !r.ok && r.status === 400 && r.error === 'window_too_large');
r = resolveEpochWindow(E, { from_ms: '1', to_ms: '1e17' }, NOW, {});
check('to_ms вне диапазона даты → 400 bad_window (а не 502 из to_timestamp)', !r.ok && r.status === 400 && r.error === 'bad_window');
r = resolveEpochWindow(E, { from_ms: '-5' }, NOW, {});
check('отрицательный from_ms → 400 bad_window', !r.ok && r.error === 'bad_window');
r = resolveEpochWindow(E, { hours: '0' }, NOW, { defaultHours: 24 });
check('hours=0 → окно по умолчанию (как parseInt(..)||24 в прежних маршрутах), часы окна согласованы', r.ok && r.window.hours === 24 && r.window.requested_from_ms === NOW - 24 * H);
r = resolveEpochWindow(E, { hours: '12abc' }, NOW, { defaultHours: 24 });
check('hours=12abc → 12 (parseInt), часы окна = 12', r.window.hours === 12 && r.window.requested_from_ms === NOW - 12 * H);
r = resolveEpochWindow(E, { hours: '9999' }, NOW, { maxHours: 168 });
check('hours ограничен потолком, часы окна = потолок', r.window.hours === 168);
r = resolveEpochWindow(E, { epoch: '2' }, NOW, { maxHours: 168 });
check('epoch=2: часы окна по длине эпохи (8 ч)', r.window.hours === 8);
check('explicit_window: false без from/to, true с ними', resolveEpochWindow(E, { hours: '6' }, NOW, {}).window.explicit_window === false
  && resolveEpochWindow(E, { from_ms: String(NOW - 3 * H) }, NOW, {}).window.explicit_window === true);
// метка немного впереди часов BFF (разброс часов Postgres и Node): окно эпохи не пустое и не отрицательное
const E2 = normalizeEpochs([{ epoch_id: 1, at_ms: NOW + 5 }]);
r = resolveEpochWindow(E2, { epoch: '1' }, NOW, {});
check('epoch=текущая при метке впереди часов BFF: to ≥ from, окно не пустое', r.ok && r.window.to_ms > r.window.effective_from_ms);
r = resolveEpochWindow(null, { hours: '24' }, NOW, { defaultHours: 24 });
check('метки недоступны: window_epochs пуст (эпоха данных неизвестна), а не «эпоха 0»', r.window.window_epochs.length === 0);

// --- блоки ответа
const blk = epochBlock(resolveEpochWindow(E, { hours: '24' }, NOW, {}));
check('epochBlock: epoch, window, reset_markers, epoch_supported', blk.epoch_supported === true && blk.epoch.epoch === 3 && blk.window.truncated_by_reset === true && Array.isArray(blk.reset_markers));
check('epochHeader: текущая эпоха; недоступно — epoch_supported=false', epochHeader(E).epoch.epoch === 3 && epochHeader(null).epoch_supported === false && epochHeader(null).epoch === null);

// --- tagEpoch
const pts = [{ ts_ms: NOW - 20 * H }, { ts_ms: NOW - 5 * H }, { ts_ms: NOW - H, epoch_id: 2 }, { ts_ms: NOW - 40 * H }];
const tg = tagEpoch(pts, E, 'ts_ms');
check('tagEpoch: эпоха определяется по времени', tg[0].epoch === 1 && tg[1].epoch === 2 && tg[3].epoch === 0);
check('tagEpoch: записанный при записи номер важнее определения по времени', tg[2].epoch === 2);
check('tagEpoch: исходные строки не меняются', pts[0].epoch === undefined && tagEpoch(null, E).length === 0);

if (failed) { console.error(`\n${failed} проверок не прошли`); process.exit(1); }
console.log('\nвсе проверки epoch.js прошли');
