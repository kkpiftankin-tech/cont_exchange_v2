// Self-contained node-тест: `node band_config.test.js`. ADR-081 — проверка настроек ширины полосы хеджа.
const assert = require('assert');
const { parseBandConfig, bandMode, normalizeBandRow, auditRecord, sanitizeActor, hmodelModeAuditRecord, HMODEL_FIELD, BAND_FIELDS, configAuditRecord, CORE_FIELDS, checkOperatorRequest } = require('./band_config');
let failed = 0;
const check = (n, c) => { if (c) console.log('[OK] ' + n); else { console.error('[FAIL] ' + n); failed++; } };

const cur = { ce_band_manual: false, ce_band_manual_q_lim: 5, ce_band_manual_q_mkt: 25, ce_band_gamma_mode: false,
  ce_band_q_floor: 2, ce_band_sigma_ref: 0.00015, ce_band_vol_ratio_min: 0.1, ce_band_vol_ratio_max: 10 };

// --- ручной режим
{
  const r = parseBandConfig({ ce_band_manual: true, ce_band_manual_q_lim: 10, ce_band_manual_q_mkt: 50 }, cur);
  check('ручной режим с допустимыми границами', r.ok && r.values.ce_band_manual === true && r.values.ce_band_manual_q_lim === 10 && r.values.ce_band_manual_q_mkt === 50);
  const r2 = parseBandConfig({ ce_band_manual: true }, cur);
  check('ручной режим без новых чисел берёт текущие границы из базы', r2.ok && r2.values.ce_band_manual === true && !('ce_band_manual_q_lim' in r2.values));
  check('Z̄mkt меньше Z̄lim отклоняется', !parseBandConfig({ ce_band_manual: true, ce_band_manual_q_lim: 30, ce_band_manual_q_mkt: 20 }, cur).ok);
  check('Z̄lim = 0 отклоняется', !parseBandConfig({ ce_band_manual: true, ce_band_manual_q_lim: 0, ce_band_manual_q_mkt: 20 }, cur).ok);
  check('больше 1000 тыс. USDT отклоняется', !parseBandConfig({ ce_band_manual_q_lim: 10, ce_band_manual_q_mkt: 1000.5 }, cur).ok);
  check('прежняя граница 100000 теперь отклоняется', !parseBandConfig({ ce_band_manual_q_lim: 10, ce_band_manual_q_mkt: 100000 }, cur).ok);
  check('нижняя 0.1 и верхняя 1000 допустимы', parseBandConfig({ ce_band_manual: true, ce_band_manual_q_lim: 0.1, ce_band_manual_q_mkt: 1000 }, cur).ok);
  check('Z̄lim ниже 0.1 (пыль) отклоняется', !parseBandConfig({ ce_band_manual: true, ce_band_manual_q_lim: 0.05, ce_band_manual_q_mkt: 20 }, cur).ok);
  check('пол выше 1000 отклоняется', !parseBandConfig({ ce_band_q_floor: 1001 }, cur).ok);
  check('нечисло отклоняется', !parseBandConfig({ ce_band_manual_q_lim: 'abc' }, cur).ok);
  check('новый Z̄lim выше текущего Z̄mkt отклоняется даже при выключенном ручном режиме',
    !parseBandConfig({ ce_band_manual_q_lim: 40 }, cur).ok);
  check('ручной режим без допустимых текущих границ отклоняется', !parseBandConfig({ ce_band_manual: true }, {}).ok);
  check('выключение ручного режима не требует границ', parseBandConfig({ ce_band_manual: false }, {}).ok);
}

// --- автоматический режим по волатильности
{
  const r = parseBandConfig({ ce_band_gamma_mode: true, ce_band_sigma_ref: 0.0002, ce_band_vol_ratio_min: 0.2, ce_band_vol_ratio_max: 5, ce_band_q_floor: 3 }, cur);
  check('автоматический режим с настройками', r.ok && r.values.ce_band_gamma_mode === true && r.values.ce_band_sigma_ref === 0.0002 && r.values.ce_band_q_floor === 3);
  check('границы множителя min > max отклоняются', !parseBandConfig({ ce_band_vol_ratio_min: 20 }, cur).ok);
  check('опорная σ = 0 отклоняется', !parseBandConfig({ ce_band_sigma_ref: 0 }, cur).ok);
  check('опорная σ больше 1 отклоняется', !parseBandConfig({ ce_band_sigma_ref: 2 }, cur).ok);
  check('булево принимается строкой', parseBandConfig({ ce_band_gamma_mode: 'true' }, cur).values.ce_band_gamma_mode === true);
  check('мусор вместо булева отклоняется', !parseBandConfig({ ce_band_gamma_mode: 'maybe' }, cur).ok);
}

// --- множитель порога арбитражёров (ADR-081)
{
  check('множитель арбитражёров 2 принимается', parseBandConfig({ ce_band_arb_scale: 2 }, cur).ok && parseBandConfig({ ce_band_arb_scale: 2 }, cur).values.ce_band_arb_scale === 2);
  check('границы множителя 0.1 и 20 допустимы', parseBandConfig({ ce_band_arb_scale: 0.1 }, cur).ok && parseBandConfig({ ce_band_arb_scale: 20 }, cur).ok);
  check('множитель ниже 0.1 отклоняется', !parseBandConfig({ ce_band_arb_scale: 0.05 }, cur).ok && !parseBandConfig({ ce_band_arb_scale: 0 }, cur).ok);
  check('множитель выше 20 отклоняется', !parseBandConfig({ ce_band_arb_scale: 21 }, cur).ok);
  check('множитель-нечисло отклоняется', !parseBandConfig({ ce_band_arb_scale: 'много' }, cur).ok);
  const rec = auditRecord({ user: 'u', before: { ...cur, ce_band_arb_scale: 1 }, after: { ce_band_arb_scale: 2 } });
  check('аудит фиксирует прежний и новый множитель', rec.changed.ce_band_arb_scale.from === 1 && rec.changed.ce_band_arb_scale.to === 2);
  check('нормализация строки из PG приводит множитель к числу', normalizeBandRow({ ce_band_arb_scale: '2.000000000000000000' }).ce_band_arb_scale === 2);
}

// --- неприсланные поля не трогаются (старый клиент не затирает новые настройки)
{
  const r = parseBandConfig({ batch_window_ms: 1000, ce_band_fee_k: 1.8 }, cur);
  check('в теле нет полей полосы — ничего не обновляем', r.ok && Object.keys(r.values).length === 0);
  const r2 = parseBandConfig(undefined, cur);
  check('пустое тело допустимо', r2.ok && Object.keys(r2.values).length === 0);
}

// --- защита от подделки запроса из браузера
{
  const json = { 'content-type': 'application/json', host: 'nik.tail318efe.ts.net' };
  check('JSON без Origin проходит (curl, не браузер)', checkOperatorRequest(json).ok);
  check('JSON с тем же хостом в Origin проходит (порт не важен)', checkOperatorRequest({ ...json, origin: 'https://nik.tail318efe.ts.net' }).ok
    && checkOperatorRequest({ 'content-type': 'application/json; charset=utf-8', host: 'localhost', origin: 'http://localhost:8091' }).ok);
  const t = checkOperatorRequest({ 'content-type': 'text/plain', host: 'x' });
  check('text/plain (простая кросс-сайтовая форма) отклоняется: 415', !t.ok && t.status === 415);
  check('нет Content-Type отклоняется', !checkOperatorRequest({ host: 'x' }).ok);
  const o = checkOperatorRequest({ ...json, origin: 'https://evil.example' });
  check('чужой Origin отклоняется: 403', !o.ok && o.status === 403 && o.code === 'bad_origin');
  check('чужой Origin из списка разрешённых проходит', checkOperatorRequest({ ...json, origin: 'https://ops.example' }, ['ops.example']).ok);
  check('мусорный Origin отклоняется', !checkOperatorRequest({ ...json, origin: 'не-урл' }).ok);
  check('X-Forwarded-Host учитывается', checkOperatorRequest({ 'content-type': 'application/json', host: 'frontend-api', 'x-forwarded-host': 'ops.example', origin: 'https://ops.example' }).ok);
}

// --- режим словами и нормализация
{
  check('режим: ручной важнее волатильности', bandMode({ ce_band_manual: true, ce_band_gamma_mode: true }) === 'manual');
  check('режим: по волатильности', bandMode({ ce_band_manual: false, ce_band_gamma_mode: true }) === 'volatility');
  check('режим: от комиссии по умолчанию', bandMode({}) === 'fee' && bandMode(null) === 'fee');
  const n = normalizeBandRow({ ce_band_manual: false, ce_band_manual_q_lim: '5.000000000000000000', ce_band_gamma_mode: true, ce_band_q_floor: '2' });
  check('NUMERIC из PG приводится к числам', n.ce_band_manual_q_lim === 5 && n.ce_band_q_floor === 2 && n.ce_band_gamma_mode === true && n.ce_band_manual === false);
}

// --- аудит
{
  const rec = auditRecord({ user: 'demo-user', before: cur, after: { ce_band_manual: true, ce_band_manual_q_lim: 10 }, nowMs: 1792400000000 });
  check('аудит: кто, режим до и после', rec.user === 'demo-user' && rec.mode_before === 'fee' && rec.mode_after === 'manual');
  check('аудит: только изменившиеся поля с прежним и новым значением',
    rec.changed.ce_band_manual.from === false && rec.changed.ce_band_manual.to === true && rec.changed.ce_band_manual_q_lim.from === 5 && rec.changed.ce_band_manual_q_lim.to === 10);
  check('аудит: нетронутые поля не попадают', !('ce_band_q_floor' in rec.changed));
  check('аудит: пользователь по умолчанию unknown', auditRecord({ before: cur, after: {} }).user === 'unknown');
}

// --- аудит основных настроек клиринга (комиссии хеджа ADR-089, порог от комиссии, skew)
{
  const base = { batch_window_ms: 1000, stale_level_ms: 60000, ce_taker_fee_bps: -1, ce_maker_fee_bps: 2, ce_inv_skew_gamma: 0.02, ce_inv_skew_max_pm: 8, slope_method: 4, ce_band_fee_k: 1.8 };
  const rec = configAuditRecord({ user: 'ops', before: base, after: { ...base, ce_maker_fee_bps: 10, ce_taker_fee_bps: 10 }, nowMs: 1792400000000 });
  check('аудит настроек: кто и обе ставки до/после', rec.user === 'ops' && rec.changed.ce_maker_fee_bps.from === 2 && rec.changed.ce_maker_fee_bps.to === 10 &&
    rec.changed.ce_taker_fee_bps.from === -1 && rec.changed.ce_taker_fee_bps.to === 10);
  check('аудит настроек: нетронутые поля не попадают', Object.keys(rec.changed).length === 2);
  check('аудит настроек: без изменения записи нет', configAuditRecord({ user: 'ops', before: base, after: base }) === null);
  const silent = configAuditRecord({ user: 'ops', before: base, after: { ...base, ce_band_fee_k: 1.8 + 0, slope_method: 4, ce_inv_skew_gamma: 0 } });
  check('аудит настроек: молчаливый сброс skew частичным POST виден', silent && silent.changed.ce_inv_skew_gamma.from === 0.02 && silent.changed.ce_inv_skew_gamma.to === 0);
  const k = configAuditRecord({ before: base, after: { ...base, ce_band_fee_k: 1.0 } });
  check('аудит настроек: порог от комиссии, пользователь unknown', k.user === 'unknown' && k.changed.ce_band_fee_k.from === 1.8 && k.changed.ce_band_fee_k.to === 1.0);
  const first = configAuditRecord({ user: 'ops', before: null, after: base });
  check('аудит настроек: первой строки не было — from = null', first.changed.ce_maker_fee_bps.from === null);
  check('аудит настроек: имя актора ограничено 120 знаками', configAuditRecord({ user: 'x'.repeat(500), before: null, after: base }).user.length === 120);
  check('аудит настроек: список полей совпадает с перезаписываемыми POST-маршрутом', CORE_FIELDS.length === 8 && CORE_FIELDS.includes('ce_maker_fee_bps') && CORE_FIELDS.includes('ce_band_fee_k'));
}

// --- ADR-088, условие 1: режим модели порогов в PG
{
  check('режим модели: on принимается', parseBandConfig({ ce_hmodel_mode: 'on' }, {}).values.ce_hmodel_mode === 'on');
  check('режим модели: регистр и пробелы нормализуются', parseBandConfig({ ce_hmodel_mode: ' Shadow ' }, {}).values.ce_hmodel_mode === 'shadow');
  check('режим модели: off принимается', parseBandConfig({ ce_hmodel_mode: 'off' }, {}).values.ce_hmodel_mode === 'off');
  check('режим модели: недопустимое значение отклоняется', !parseBandConfig({ ce_hmodel_mode: 'auto' }, {}).ok && !parseBandConfig({ ce_hmodel_mode: 'true' }, {}).ok);
  check('режим модели: не прислан — поля в значениях нет (не меняется)', !('ce_hmodel_mode' in parseBandConfig({ ce_band_arb_scale: 2 }, {}).values));
  check('режим модели: пустая строка = не прислан', !('ce_hmodel_mode' in parseBandConfig({ ce_hmodel_mode: '' }, {}).values));
  check('режим модели: не входит в список полей ширины полосы (свой аудит и своя запись)', !BAND_FIELDS.includes(HMODEL_FIELD));
  check('режим модели: строка из PG сохраняется строкой, NULL — null, нет колонки — поля нет',
    normalizeBandRow({ ce_hmodel_mode: 'on' }).ce_hmodel_mode === 'on' && normalizeBandRow({ ce_hmodel_mode: null }).ce_hmodel_mode === null
    && !('ce_hmodel_mode' in normalizeBandRow({ ce_band_manual: false })));
  const a = hmodelModeAuditRecord({ user: 'ops', before: 'shadow', after: 'on', nowMs: 0 });
  check('аудит режима: прежнее → новое и актор', a.user === 'ops' && a.changed.ce_hmodel_mode.from === 'shadow' && a.changed.ce_hmodel_mode.to === 'on');
  check('аудит режима: не было значения — from = null', hmodelModeAuditRecord({ user: 'ops', before: null, after: 'shadow' }).changed.ce_hmodel_mode.from === null);
  check('аудит режима: без изменения записи нет', hmodelModeAuditRecord({ user: 'ops', before: 'on', after: 'on' }) === null);
  check('аудит режима: не прислан — записи нет', hmodelModeAuditRecord({ user: 'ops', before: 'on', after: undefined }) === null);
  check('актор: зарезервированные префиксы автоматики помечаются unverified:',
    sanitizeActor('ledger:auto-fuse') === 'unverified:ledger:auto-fuse' && sanitizeActor(' System : x') === 'unverified: System : x' && sanitizeActor('BFF:ops').startsWith('unverified:'));
  check('актор: обычные имена и пустое значение не меняются', sanitizeActor('alex') === 'alex' && sanitizeActor('') === 'unknown' && sanitizeActor(undefined) === 'unknown'
    && sanitizeActor('ledgerx') === 'ledgerx' && sanitizeActor('a@b.c') === 'a@b.c');
  check('актор: помеченное значение не длиннее 120 знаков', sanitizeActor('ledger:' + 'x'.repeat(500)).length === 120);
  check('аудит режима: подмена актора ledger:auto-fuse помечена во всех трёх журналах',
    hmodelModeAuditRecord({ user: 'ledger:auto-fuse', before: 'on', after: 'shadow' }).user.startsWith('unverified:')
    && auditRecord({ user: 'ledger:auto-fuse', before: {}, after: { ce_band_arb_scale: 2 } }).user.startsWith('unverified:')
    && configAuditRecord({ user: 'ledger:auto-fuse', before: null, after: { ce_band_fee_k: 1 } }).user.startsWith('unverified:'));
  check('аудит режима: подтверждение после предохранителя фиксируется в записи',
    hmodelModeAuditRecord({ user: 'ops', before: 'shadow', after: 'on', confirmedAfterFuse: true }).confirmed_after_fuse === true
    && !('confirmed_after_fuse' in hmodelModeAuditRecord({ user: 'ops', before: 'shadow', after: 'on' })));
  check('аудит режима: актор по умолчанию unknown, длина ограничена', hmodelModeAuditRecord({ before: null, after: 'on' }).user === 'unknown'
    && hmodelModeAuditRecord({ user: 'x'.repeat(500), before: null, after: 'on' }).user.length === 120);
}

assert.strictEqual(failed, 0, failed + ' failed');
console.log('band_config tests passed');
