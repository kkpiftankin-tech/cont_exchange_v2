// Self-contained node-тест: `node band_config.test.js`. ADR-081 — проверка настроек ширины полосы хеджа.
const assert = require('assert');
const { parseBandConfig, bandMode, normalizeBandRow, auditRecord, checkOperatorRequest } = require('./band_config');
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

assert.strictEqual(failed, 0, failed + ' failed');
console.log('band_config tests passed');
