// Self-contained node-тест: `node hedge_model_dto.test.js` (у frontend/api нет jest).
const { decOrNull, buildHedgeModelDto, bandSamples, minuteBucketMs, historyCsv, CSV_COLUMNS, BAND_HISTORY_DDL } = require('./hedge_model_dto');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}
const dec = (units, scale = 6) => ({ units: String(units), scale });

// --- decOrNull: ноль ≠ нет значения
check('Decimal 15971000 / 6 → 15,971', decOrNull(dec('15971000')) === 15.971);
check('Decimal с нулём → 0 (законное значение)', decOrNull(dec('0')) === 0);
check('нет поля → null (не 0)', decOrNull(null) === null && decOrNull(undefined) === null);
check('мусор → null', decOrNull({ units: 'x', scale: 6 }) === null);

// --- buildHedgeModelDto
check('нет блока (режим off) → null', buildHedgeModelDto(null, 1) === null && buildHedgeModelDto(undefined, 1) === null);
const tr = buildHedgeModelDto({
  computed: true, applied: false, mode: 'shadow', reason: '', regime: 'HEDGE_MODEL_REGIME_NOISE',
  b_m: dec('15971000'), b_t: dec('20215000'), sz_est: dec('2000000'), mu_est: dec('-120000'), theta_est: dec('0'),
  peclet: 0.5, sigma_bps: dec('4800000'), fee_maker_bps: dec('10000000'), fee_taker_bps: dec('10000000'),
  est_minutes: 0, params_hash: 123456, computed_at_ms: '1000', transfer_threshold: null, transfer_target: null,
}, 1500);
check('переводчик: пороги и режим', tr.b_m === 15.971 && tr.b_t === 20.215 && tr.regime === 'noise' && tr.mode === 'shadow');
check('переводчик: порогов перевода нет (null, не 0)', tr.transfer_threshold === null && tr.transfer_target === null);
check('оценки: шум 2, дрейф знаковый −0,12, возврат 0', tr.sz_est === 2 && tr.mu_est === -0.12 && tr.theta_est === 0);
check('est_minutes 0 сохранён — шум по умолчанию, а не оценка', tr.est_minutes === 0);
check('возраст расчёта считает BFF', tr.age_ms === 500 && tr.computed_at_ms === 1000);
check('причины нет, когда посчитано', tr.reason === null);
const held = buildHedgeModelDto({ computed: true, mode: 'on', reason: 'budget_hold', b_m: dec('15971000'), b_t: dec('20215000'), regime: 'HEDGE_MODEL_REGIME_NOISE' }, 1);
check('удержано при исчерпанном бюджете: пороги есть, причина budget_hold видна', held.computed === true && held.reason === 'budget_hold' && held.b_t === 20.215);

const rebate = buildHedgeModelDto({ computed: true, mode: 'on', applied: true, regime: 'HEDGE_MODEL_REGIME_DRIFT', b_m: dec('0'), b_t: dec('6142400') }, 0);
check('ребейт: b_m = 0 остаётся нулём (присутствует)', rebate.b_m === 0 && rebate.b_t === 6.1424 && rebate.applied === true && rebate.regime === 'drift');

const link = buildHedgeModelDto({ computed: true, mode: 'shadow', transfer_threshold: dec('41557000'), transfer_target: dec('0'), regime: 'HEDGE_MODEL_REGIME_UNSPECIFIED' }, 0);
check('связка: порог 41,557 и цель 0 присутствуют, порогов переводчика нет', link.transfer_threshold === 41.557 && link.transfer_target === 0 && link.b_m === null && link.b_t === null && link.regime === null);

const miss = buildHedgeModelDto({ computed: false, mode: 'shadow', reason: 'no_sigma', b_m: dec('15971000'), peclet: 3 }, 0);
check('не посчитано: причина видна, пороги и режим скрыты', miss.computed === false && miss.reason === 'no_sigma' && miss.b_m === null && miss.peclet === null && miss.regime === null);
check('неизвестный режим → null', buildHedgeModelDto({ computed: true, regime: 'HEDGE_MODEL_REGIME_X' }, 0).regime === null);
check('Infinity в числе Пекле → null', buildHedgeModelDto({ computed: true, peclet: Infinity }, 0).peclet === null);

// --- bandSamples
const rows = [
  { agent_id: 'T_BTC_binance', agent_kind: 'translator', c_position: 12.5, in_flight: 3, z_lim: 18, z_mkt: 18, hmodel: tr },
  { agent_id: 'T_ETH_okx', agent_kind: 'translator', c_position: 1, z_lim: 18, z_mkt: 18, hmodel: null },        // режим off → пропуск
  { agent_id: 'A_BTC_bo', agent_kind: 'arbitrageur', c_position: null, z_lim: null, z_mkt: null, hmodel: link },
  { agent_id: '', hmodel: tr },
  null,
];
const s = bandSamples(rows, 1700000040000);
check('в историю идут агенты с моделью: 2 из 5', s.length === 2);
check('рабочие и модельные пороги рядом', s[0].z_lim === 18 && s[0].b_m === 15.971 && s[0].b_t === 20.215 && s[0].position === 12.5);
check('нет данных остаётся null, а не нулём', s[1].position === null && s[1].z_lim === null && s[1].transfer_target === 0);
check('метка времени общая', s.every((x) => x.ts_ms === 1700000040000));
check('не массив → пусто', bandSamples(null, 1).length === 0);

// --- minuteBucketMs
check('минутная корзина', minuteBucketMs(1700000045123) === 1700000040000);

// --- CSV
const csv = historyCsv([{ ts: new Date(1700000040000), agent_id: 'T_BTC_binance', kind: 'translator', mode: 'shadow', position: 12.5, reason: 'a,b', computed: true, b_m: 0 }]);
const lines = csv.trim().split('\n');
check('CSV: заголовок из колонок', lines[0] === CSV_COLUMNS.join(','));
check('CSV: время ISO, ноль сохранён, запятая в тексте экранирована', lines[1].startsWith('2023-11-14T22:14:00.000Z,T_BTC_binance') && lines[1].includes(',0,') && lines[1].includes('"a,b"'));
check('CSV: пустое значение — пустая ячейка', lines[1].split(',')[5] === '');
check('CSV: перевод строки \\r в тексте экранируется кавычками', historyCsv([{ agent_id: 'x', reason: 'a\rb' }]).includes('"a\rb"'));

// --- DDL
check('DDL: ключ (агент, минута) и числовые типы денег', BAND_HISTORY_DDL.includes('PRIMARY KEY (agent_id, ts)') && BAND_HISTORY_DDL.includes('NUMERIC(38, 12)'));

if (failed) { console.error(failed + ' failed'); process.exit(1); }
console.log('ALL OK');
