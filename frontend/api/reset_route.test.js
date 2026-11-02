// Self-contained node-тест маршрутов сброса позиций и меток (ADR-090, CR-F18-018, Э1): `node reset_route.test.js`.
// server.js загружается в этот же процесс с подменой `pg` (заглушка с настоящей семантикой транзакции: метка видна после COMMIT и пропадает при
// ROLLBACK) и gRPC-клиента ledger; реальные Postgres, ledger и сеть не затрагиваются. Проверяются: валидация и отказы (с записью в аудит), запрет
// вне песочницы, порядок «аудит попытки → ledger → метка → аудит исхода», откат при отказе ledger, сбой записи метки после ledger, идемпотентность
// и конфликт request_id, защита от двойного нажатия и гонки, интервал между сбросами, маршрут /epochs, окна по метке в маршрутах истории.
const Module = require('module');
const http = require('http');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}

const H = 3600 * 1000;
const T0 = Date.now();

// ---- заглушка Postgres ----
const state = {
  epochs: [   // зафиксированные метки
    { epoch_id: 1, at_ms: T0 - 30 * H, kind: 'legacy', note: 'legacy', actor: 'migration', boot_id: null, request_id: 'legacy-1' },
    { epoch_id: 2, at_ms: T0 - 10 * H, kind: 'bridge', note: 'опыт B', actor: 'owner', boot_id: 'b2', request_id: 'req-2' },
    { epoch_id: 3, at_ms: T0 - 2 * H, kind: 'absolute', note: 'опыт C', actor: 'owner', boot_id: 'b3', request_id: 'req-3' },
  ],
  queries: [],
  events: [],            // порядок значимых событий: audit:<action>, ledger:reset, sql:epoch_open, sql:commit, sql:rollback
  audits: [],            // записи operator_audit_log
  failEpochOpen: 0,      // сколько ближайших вызовов ce_epoch_open должны упасть
  ledgerDelayMs: 0,
};
function fakeQuery(sql, params, client) {
  const text = String(sql);
  state.queries.push({ sql: text, params: params || [] });
  const p = params || [];
  if (/^\s*BEGIN/.test(text)) { client.pending = []; client.inTx = true; return Promise.resolve({ rows: [] }); }
  if (/^\s*COMMIT/.test(text)) { state.events.push('sql:commit'); for (const e of client.pending) state.epochs.push(e); client.pending = []; client.inTx = false; return Promise.resolve({ rows: [] }); }
  if (/^\s*ROLLBACK/.test(text)) { state.events.push('sql:rollback'); client.pending = []; client.inTx = false; return Promise.resolve({ rows: [] }); }
  if (/SET LOCAL|pg_advisory_xact_lock/.test(text)) return Promise.resolve({ rows: [] });
  if (/ce_epoch_backfill_legacy/.test(text)) return Promise.resolve({ rows: [{ n: 0 }] });
  if (/FROM ce_reset_epoch ORDER BY epoch_id/.test(text)) {
    return Promise.resolve({ rows: state.epochs.map((e) => ({ epoch_id: String(e.epoch_id), at_ms: String(e.at_ms), kind: e.kind, note: e.note, actor: e.actor, boot_id: e.boot_id })) });
  }
  if (/FROM ce_reset_epoch WHERE request_id = \$1/.test(text)) {
    const e = state.epochs.find((x) => x.request_id === p[0]);
    return Promise.resolve({ rows: e ? [{ epoch_id: String(e.epoch_id), at_ms: String(e.at_ms), kind: e.kind, note: e.note, actor: e.actor }] : [] });
  }
  if (/COALESCE\(max\(epoch_id\), 0\) AS cur/.test(text)) {
    const last = state.epochs[state.epochs.length - 1];
    return Promise.resolve({ rows: [{ cur: String(last ? last.epoch_id : 0), too_recent: !!last && (Date.now() - last.at_ms) < 60000, in_future: !!last && last.at_ms > Date.now() }] });
  }
  if (/FROM ce_epoch_open\(/.test(text)) {
    state.events.push('sql:epoch_open');
    if (state.failEpochOpen > 0) { state.failEpochOpen--; return Promise.reject(new Error('epoch open failed (test)')); }
    const e = { epoch_id: state.epochs.length + client.pending.length + 1, at_ms: Date.now(), kind: 'bridge', note: p[1], actor: p[0], boot_id: p[4], request_id: p[2], before: p[3] };
    client.pending.push(e);
    return Promise.resolve({ rows: [{ epoch_id: String(e.epoch_id), at_ms: String(e.at_ms), kind: e.kind, note: e.note }] });
  }
  if (/INSERT INTO operator_audit_log/.test(text)) { state.events.push('audit:' + p[1]); state.audits.push({ actor: p[0], action: p[1], ip: p[2], details: JSON.parse(p[4]) }); return Promise.resolve({ rows: [], rowCount: 1 }); }
  return Promise.resolve({ rows: [], rowCount: 0 });
}
class FakePool {
  query(sql, params) { return fakeQuery(sql, params, { pending: [] }); }
  async connect() { const c = { pending: [], inTx: false, query: (sql, params) => fakeQuery(sql, params, c), release() {} }; return c; }
  on() {}
}

// ---- заглушка gRPC-клиента ledger ----
const ledgerCalls = [];
let ledgerFail = false;
class FakeLedger {
  constructor() {
    for (const m of ['ResetAgentPositions', 'GetExchangeBalances', 'GetAgentPositions', 'GetAgentPnl', 'GetNodeBalances']) {
      this[m] = (req, opts, cb) => {
        ledgerCalls.push({ method: m, req });
        if (m === 'ResetAgentPositions') {
          state.events.push('ledger:reset');
          const done = () => (ledgerFail ? cb(new Error('ledger down (test)')) : cb(null, { cleared: 68 }));
          return state.ledgerDelayMs ? setTimeout(done, state.ledgerDelayMs) : done();
        }
        if (m === 'GetExchangeBalances') {
          const d = (u, sc) => ({ units: String(u), scale: sc });
          // BTC: 10.00000000; USDT: больше 2^53 в единицах — проверка, что деньги в снимке не проходят через double
          return cb(null, { boot_id: 'boot-X', balances: [
            { currency: 'BTC', assets_total: d('1000000000', 8), client_liability: d('200000000', 8), venue_total: d('100000000', 8), in_flight: d(0, 8) },
            { currency: 'USDT', assets_total: d('9007199254740993', 0), client_liability: d(0, 0), venue_total: d(0, 0), in_flight: d(0, 0) },
          ] });
        }
        return cb(null, {});
      };
    }
  }
}
const nsProxy = (path) => new Proxy({}, {
  get: (_t, k) => {
    if (path.join('.') === 'fob.ledger.v1' && k === 'LedgerService') return FakeLedger;
    if (typeof k !== 'string') return undefined;
    return k === 'v1' || path.length < 3 ? nsProxy(path.concat(k)) : class { constructor() { return new Proxy({}, { get: () => (r, o, cb) => cb(new Error('not mocked')) }); } };
  },
});

const origLoad = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === 'pg') return { Pool: FakePool };
  const m = origLoad.apply(this, arguments);
  // У модуля gRPC loadPackageDefinition — геттер, присвоить нельзя: подменяем через прокси, остальное отдаём как есть.
  if (request === '@grpc/grpc-js') return new Proxy(m, { get: (t, k) => (k === 'loadPackageDefinition' ? () => nsProxy([]) : t[k]) });
  return m;
};

const PORT = 19090 + Math.floor(Math.random() * 500);
process.env.PORT = String(PORT);
process.env.FRONTEND_POSTGRES_DSN = 'postgres://fake/fake';
process.env.CE_SURPLUS_POLL_MS = '0';
delete process.env.CE_RESET_ALLOWED;
const realLog = console.log;
console.log = () => {};
require('./server.js');
console.log = realLog;

function call(method, path, body, headers) {
  return new Promise((resolve, reject) => {
    const payload = body === undefined ? null : JSON.stringify(body);
    const req = http.request({ host: '127.0.0.1', port: PORT, path, method, headers: { ...(payload ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) } : {}), 'x-user-id': 'tester', ...(headers || {}) } }, (res) => {
      let buf = '';
      res.on('data', (c) => { buf += c; });
      res.on('end', () => { let j = null; try { j = JSON.parse(buf); } catch (_) { /* не JSON */ } resolve({ status: res.statusCode, json: j, raw: buf }); });
    });
    req.on('error', reject);
    if (payload) req.write(payload);
    req.end();
  });
}
const resetCalls = () => ledgerCalls.filter((c) => c.method === 'ResetAgentPositions');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const lastEpoch = () => state.epochs[state.epochs.length - 1];
const ageLastEpoch = () => { lastEpoch().at_ms -= 120000; };   // метка «старше минуты»: следующий сброс допустим
const body = (extra) => ({ note: 'опыт D', confirm: true, request_id: 'rq-' + Math.random().toString(36).slice(2, 12), expected_epoch: lastEpoch().epoch_id, ...extra });
const RESET = '/api/vector-clearing/reset-positions';

(async () => {
  for (let i = 0; i < 40; i++) { try { await call('GET', '/healthz'); break; } catch (_) { await sleep(50); } }

  // --- валидация и отказы: ledger не вызывается, каждый отказ пишется в аудит
  let r = await call('POST', RESET, undefined, { 'Content-Type': 'text/plain' });
  check('без JSON Content-Type → 415, ledger не вызывается, отказ записан в аудит', r.status === 415 && resetCalls().length === 0 && state.audits.some((a) => a.action === 'ce_reset_positions_denied' && a.details.status === 415));
  const bad = async (name, extra, status, code) => {
    const n = state.audits.length;
    const rr = await call('POST', RESET, body(extra));
    check(`${name} → ${status} ${code}, ledger не вызывается, отказ в аудите`, rr.status === status && rr.json.error === code && resetCalls().length === 0 && state.audits.length === n + 1 && state.audits[n].action === 'ce_reset_positions_denied');
  };
  await bad('выборочный сброс', { agent_ids: ['T_BTC_binance'] }, 400, 'selective_reset_not_supported');
  await bad('без названия эксперимента', { note: undefined }, 400, 'note_required');
  await bad('название из пробелов', { note: '   ' }, 400, 'note_required');
  await bad('название длиннее 200 знаков', { note: 'я'.repeat(201) }, 400, 'note_required');
  await bad('NUL в названии', { note: 'опыт\u0000D' }, 400, 'note_required');
  await bad('перевод строки в названии', { note: 'опыт\nD' }, 400, 'note_required');
  await bad('название не строка', { note: { a: 1 } }, 400, 'note_required');
  await bad('без подтверждения', { confirm: undefined }, 400, 'confirm_required');
  await bad('request_id короче 8 знаков', { request_id: 'abc' }, 400, 'bad_request_id');
  await bad('request_id с запрещёнными знаками', { request_id: 'bad id with spaces' }, 400, 'bad_request_id');
  await bad('request_id — объект (не превращается в «[object Object]»)', { request_id: { a: 1 } }, 400, 'bad_request_id');
  await bad('NUL в request_id', { request_id: 'abcdefgh\u0000x' }, 400, 'bad_request_id');
  await bad('без expected_epoch', { expected_epoch: undefined }, 400, 'expected_epoch_required');
  await bad('expected_epoch не число', { expected_epoch: 'abc' }, 400, 'expected_epoch_required');
  r = await call('GET', RESET);
  check('GET на маршруте сброса → 405', r.status === 405);
  await bad('устаревший экран (expected_epoch ≠ текущей)', { expected_epoch: 2 }, 409, 'epoch_changed');

  // --- запрет вне песочницы
  process.env.CE_RESET_ALLOWED = '0';
  let n = state.audits.length;
  r = await call('POST', RESET, body());
  check('CE_RESET_ALLOWED=0 → 403 reset_not_allowed, ledger не вызывается, отказ в аудите', r.status === 403 && r.json.error === 'reset_not_allowed' && resetCalls().length === 0 && state.audits.length === n + 1);
  delete process.env.CE_RESET_ALLOWED;

  // --- сбой ledger: метка не создаётся, транзакция откатывается, исход в аудите
  ledgerFail = true;
  state.events.length = 0;
  const before = state.epochs.length;
  r = await call('POST', RESET, body());
  check('сбой ledger → 502 grpc_error, эпоха не создана, транзакция откатилась', r.status === 502 && r.json.error === 'grpc_error' && state.epochs.length === before && state.events.includes('sql:rollback') && !state.events.includes('sql:epoch_open'));
  check('сбой ledger записан в аудит (stage=ledger), попытка записана до вызова ledger', state.audits.some((a) => a.action === 'ce_reset_positions_failed' && a.details.stage === 'ledger')
    && state.events.indexOf('audit:ce_reset_positions_attempt') >= 0 && state.events.indexOf('audit:ce_reset_positions_attempt') < state.events.indexOf('ledger:reset'));
  ledgerFail = false;

  // --- успешный сброс
  state.queries.length = 0; state.events.length = 0;
  const callsBefore = resetCalls().length;
  r = await call('POST', RESET, body({ request_id: 'rq-success-1' }));
  check('сброс → 200, эпоха 4, вид bridge, absolute=false', r.status === 200 && r.json.epoch === 4 && r.json.kind === 'bridge' && r.json.absolute === false);
  check('в ответе название, request_id, число сброшенных, предупреждение про остатки площадок', r.json.note === 'опыт D' && r.json.request_id === 'rq-success-1' && r.json.cleared === 68 && /absolute_reset\.sh/.test(r.json.warning));
  check('ledger вызван один раз, с пустым списком агентов (общий сброс)', resetCalls().length === callsBefore + 1 && resetCalls().slice(callsBefore).every((c) => Array.isArray(c.req.agent_ids) && c.req.agent_ids.length === 0));
  check('порядок: аудит попытки → ledger → метка → фиксация → аудит исхода', (() => {
    const e = state.events;
    const ia = e.indexOf('audit:ce_reset_positions_attempt'), il = e.indexOf('ledger:reset'), io = e.indexOf('sql:epoch_open'), ic = e.indexOf('sql:commit'), is = e.indexOf('audit:ce_reset_positions');
    return ia >= 0 && ia < il && il < io && io < ic && ic < is;
  })());
  const open = state.queries.find((q) => /FROM ce_epoch_open\(/.test(q.sql));
  check('метка открыта функцией Postgres с актором, названием и request_id', !!open && open.params[0] === 'tester' && open.params[1] === 'опыт D' && open.params[2] === 'rq-success-1');
  const beforeState = open ? JSON.parse(open.params[3]) : {};
  check('снимок до сброса в метке: boot_id ledger, остатки по валютам точными строками (без double)',
    beforeState.source === 'bff-bridge' && beforeState.boot_id === 'boot-X' && beforeState.exchange.BTC.assets === '10.00000000'
    && beforeState.exchange.USDT.assets === '9007199254740993' && beforeState.exchange.BTC.client_liability === '2.00000000');
  check('boot_id ledger передан в метку отдельным параметром', open && open.params[4] === 'boot-X');
  check('запасная метка конфигурации обновлена моментом эпохи (откат на прежнюю версию)', state.queries.some((q) => /INSERT INTO f05a_clearing_config/.test(q.sql) && Number(q.params[0]) === state.epochs[3].at_ms));
  check('база прибыли системы удалена (будет создана заново)', state.queries.some((q) => /DELETE FROM ce_system_pnl_baseline/.test(q.sql)));
  const okAudit = state.audits.find((a) => a.action === 'ce_reset_positions');
  check('аудит исхода: актор, эпоха, название, request_id', !!okAudit && okAudit.actor === 'tester' && okAudit.details.epoch === 4 && okAudit.details.note === 'опыт D' && okAudit.details.request_id === 'rq-success-1');

  // --- интервал между сбросами
  r = await call('POST', RESET, body());
  check('второй сброс сразу после первого → 429 reset_too_frequent, ledger не вызывается', r.status === 429 && r.json.error === 'reset_too_frequent' && resetCalls().length === callsBefore + 1);

  // --- повтор того же запроса и конфликт request_id
  const calls = resetCalls().length;
  r = await call('POST', RESET, { note: 'опыт D', confirm: true, request_id: 'rq-success-1', expected_epoch: 4 });
  check('повтор с тем же request_id, актором и названием → 200 replay, нового сброса нет', r.status === 200 && r.json.replay === true && r.json.epoch === 4 && resetCalls().length === calls && state.epochs.length === 4);
  r = await call('POST', RESET, { note: 'другое название', confirm: true, request_id: 'rq-success-1', expected_epoch: 4 });
  check('тот же request_id с другим названием → 409 request_id_conflict (чужой запрос не подменяет сброс)', r.status === 409 && r.json.error === 'request_id_conflict' && resetCalls().length === calls);

  // --- гонка: два сброса одновременно
  ageLastEpoch();
  state.ledgerDelayMs = 250;
  const callsBeforeRace = resetCalls().length;
  const exp = lastEpoch().epoch_id;
  const [a, b] = await Promise.all([
    call('POST', RESET, { note: 'гонка', confirm: true, request_id: 'rq-race-aaaa', expected_epoch: exp }),
    call('POST', RESET, { note: 'гонка', confirm: true, request_id: 'rq-race-bbbb', expected_epoch: exp }),
  ]);
  state.ledgerDelayMs = 0;
  const codes = [a.status, b.status].sort();
  check('два одновременных сброса → один 200, другой 409 reset_in_progress; ledger сброшен один раз',
    codes.join() === '200,409' && [a, b].some((x) => x.json.error === 'reset_in_progress') && resetCalls().length === callsBeforeRace + 1);

  // --- сбой записи метки после сброса в ledger
  ageLastEpoch();
  state.failEpochOpen = 1;
  state.audits.length = 0;
  const epochsBefore = state.epochs.length;
  r = await call('POST', RESET, body({ request_id: 'rq-nometa-1' }));
  check('сбой записи метки → 502 epoch_not_recorded с request_id и числом сброшенных (прятать нельзя)', r.status === 502 && r.json.error === 'epoch_not_recorded' && r.json.request_id === 'rq-nometa-1' && r.json.cleared === 68);
  check('после сбоя метка не создана, а сбой записан в аудит (stage=epoch_after_ledger_reset)', state.epochs.length === epochsBefore
    && state.audits.some((x) => x.action === 'ce_reset_positions_failed' && x.details.stage === 'epoch_after_ledger_reset' && x.details.cleared === 68));
  r = await call('POST', RESET, { note: 'опыт D', confirm: true, request_id: 'rq-nometa-1', expected_epoch: lastEpoch().epoch_id });
  check('повтор с тем же request_id дописывает метку', r.status === 200 && state.epochs.length === epochsBefore + 1);

  // --- /epochs
  r = await call('GET', '/api/vector-clearing/epochs?limit=2');
  const cnt = state.epochs.length;
  check('/epochs: supported, текущая эпоха, новые первыми, has_more', r.status === 200 && r.json.supported === true && r.json.current.epoch === cnt
    && r.json.epochs.length === 2 && r.json.epochs[0].epoch === cnt && r.json.epochs[1].epoch === cnt - 1 && r.json.has_more === true && r.json.count === cnt);
  check('/epochs не отдаёт автора и boot_id (подменяемые/служебные поля остаются в базе)', !JSON.stringify(r.json).includes('actor') && !JSON.stringify(r.json).includes('boot_id') && !JSON.stringify(r.json).includes('tester'));

  // --- окна по метке в маршрутах истории
  await sleep(30);   // метка создана «только что»: без паузы запрос мог попасть в ту же миллисекунду, а граница окна строго меньше его конца
  const cur = lastEpoch();
  state.queries.length = 0;
  r = await call('GET', '/api/vector-clearing/pnl-history?scope=all&hours=24');
  const pq = state.queries.find((q) => /FROM ce_pnl_totals_history/.test(q.sql));
  check('pnl-history: окно усечено до начала текущей эпохи, блок epoch в ответе', r.status === 200 && r.json.epoch.epoch === cur.epoch_id && r.json.window.truncated_by_reset === true && r.json.window.crosses_reset === true
    && pq && Number(pq.params[1]) === cur.at_ms && r.json.since_ms === cur.at_ms);
  state.queries.length = 0;
  r = await call('GET', '/api/vector-clearing/pnl-history?scope=all&epoch=2');
  const pq2 = state.queries.find((q) => /FROM ce_pnl_totals_history/.test(q.sql));
  check('pnl-history?epoch=2: вся эпоха 2 — от её метки до метки эпохи 3', r.status === 200 && pq2 && Number(pq2.params[1]) === state.epochs[1].at_ms && Number(pq2.params[2]) === state.epochs[2].at_ms && r.json.window.epoch_requested === 2);
  r = await call('GET', '/api/vector-clearing/pnl-history?scope=all&epoch=2&hours=5');
  check('epoch вместе с hours → 400 ambiguous_window', r.status === 400 && r.json.error === 'ambiguous_window');
  r = await call('GET', '/api/vector-clearing/pnl-history?scope=all&epoch=99');
  check('неизвестная эпоха → 404 epoch_not_found', r.status === 404 && r.json.error === 'epoch_not_found');
  r = await call('GET', '/api/vector-clearing/pnl-history?scope=all&epoch=abc');
  check('epoch=abc → 400 bad_epoch (а не молчаливое окно текущей эпохи)', r.status === 400 && r.json.error === 'bad_epoch');
  r = await call('GET', `/api/vector-clearing/pnl-history?scope=all&from_ms=1&to_ms=${Date.now()}`);
  check('явное окно длиннее потолка → 400 window_too_large (а не запрос на весь срок хранения)', r.status === 400 && r.json.error === 'window_too_large');
  r = await call('GET', '/api/vector-clearing/pnl-history?scope=all&hours=48&allow_cross_reset=true');
  check('allow_cross_reset: окно не усечено, видны затронутые эпохи', r.status === 200 && r.json.window.truncated_by_reset === false && r.json.window.crosses_reset === true && r.json.window.window_epochs.length >= 3);
  r = await call('GET', '/api/vector-clearing/pair-trades-history?hours=48&allow_cross_reset=true');
  check('pair-trades-history: окно через метку не поддержано (накопления склеили бы эксперименты) → 400', r.status === 400 && r.json.error === 'cross_reset_not_supported');
  state.queries.length = 0;
  r = await call('GET', '/api/vector-clearing/pair-trades-history?hours=24');
  const tq = state.queries.find((q) => /FROM child_orders/.test(q.sql) && /\$6/.test(q.sql));
  check('pair-trades-history: запрос к child_orders идёт с границами окна по метке', r.status === 200 && !!tq && Number(tq.params[1]) === cur.at_ms && r.json.since_ms === cur.at_ms && r.json.epoch.epoch === cur.epoch_id);
  // явные from_ms/to_ms внутри одной эпохи (эпоха 2): границы закреплены, окно не подменяется «последними N часами»
  state.queries.length = 0;
  const f2 = state.epochs[1].at_ms + H, t2 = state.epochs[1].at_ms + 3 * H;
  r = await call('GET', `/api/vector-clearing/pair-trades-history?from_ms=${f2}&to_ms=${t2}`);
  const xq = state.queries.find((q) => /FROM child_orders/.test(q.sql) && /\$6/.test(q.sql));
  check('pair-trades-history с from_ms/to_ms: границы запроса и since_ms равны заданным, а не now − часы', r.status === 200 && !!xq && Number(xq.params[1]) === f2 && Number(xq.params[5]) === t2 && r.json.since_ms === f2 && r.json.window.explicit_window === true);
  check('window_hours согласован с длиной окна (2 ч)', r.json.window_hours === 2);

  if (failed) { console.error(`\n${failed} проверок не прошли`); process.exit(1); }
  console.log('\nвсе проверки маршрутов сброса и меток прошли');
  process.exit(0);
})().catch((e) => { console.error('тест упал:', e); process.exit(2); });
