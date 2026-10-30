'use strict';
// Прибыль системы CE по реальным остаткам (CALC-CE-SYSTEM-PNL, ADR-083).
// Остатки биржи по валютам q_c (ledger GetExchangeBalances: активы минус обязательства перед клиентами) оцениваются по рыночным ценам
// m_c (USDT). Изменение стоимости остатков с момента базы раскладывается ТОЧНО на две части:
//   торговый результат  T = Σ (q_c − q0_c)·m_c(t)      — что принесли сделки (изменение остатков по текущим ценам);
//   переоценка запасов  V = Σ q0_c·(m_c(t) − m0_c)     — как изменилась стоимость запасов, стоявших на балансе в момент базы.
//   T + V = Σ q_c·m_c(t) − Σ q0_c·m0_c  (изменение стоимости остатков).
// Прежняя оценка «стоимость остатков минус стартовый капитал» смешивала обе части: на засеянном балансе в 1,95 млн USDT она за секунды
// прыгала на сотни USDT только от движения рынка. Чистые функции без ввода-вывода; запрос ledger/цен и запись в PG — в server.js.

const finite = (v) => v !== null && v !== undefined && Number.isFinite(Number(v));

// Цена актива в USDT: МЕДИАНА по биржам цены пары ASSET/USDT из последних сегментов клиринга ({ venue_id, pair, mid, event_time_ms }); USDT = 1.
// Медиана и фильтр свежести нужны, чтобы состав бирж (тонкие кривые выпадают и возвращаются) не двигал цену на межбиржевой спред.
// Сегмент старше maxAgeMs относительно nowMs не используется (nowMs не задан — фильтра нет). Возвращает { ccy: цена } только для валют с ценой.
function assetUsdMids(segRows, assets, nowMs, maxAgeMs = 180000) {
  const prices = new Map();
  for (const r of Array.isArray(segRows) ? segRows : []) {
    if (!r || !r.pair || !finite(r.mid) || !(Number(r.mid) > 0)) continue;
    if (finite(nowMs) && finite(r.event_time_ms) && Number(nowMs) - Number(r.event_time_ms) > maxAgeMs) continue;
    const m = /^([A-Z0-9]+)\/USDT$/.exec(String(r.pair));
    if (!m) continue;
    if (!prices.has(m[1])) prices.set(m[1], []);
    prices.get(m[1]).push(Number(r.mid));
  }
  const median = (xs) => { const t = xs.slice().sort((x, y) => x - y); const n = t.length; return n % 2 ? t[(n - 1) / 2] : (t[n / 2 - 1] + t[n / 2]) / 2; };
  const out = {};
  for (const a of Array.isArray(assets) ? assets : []) {
    if (a === 'USDT') { out.USDT = 1; continue; }
    const xs = prices.get(a);
    if (xs && xs.length) out[a] = median(xs);
  }
  return out;
}

// Решение о базе по состоянию:
//   'keep'   — база есть, относится к текущему сбросу статистики и к текущему запуску ledger;
//   'create' — базы нет или был сброс статистики сделок: новая база, накопленное обнуляется;
//   'rebase' — перезапущен ledger (сменился boot_id): остатки бирж он держит в памяти и обнуляет при перезапуске, поэтому разрыв остатков
//              нельзя выдавать за торговый результат; новая база, накопленное переносится (carry);
//   'wait'   — базу пока задавать нельзя (нет остатков или нет цены у валюты с остатком): иначе переоценка запасов навсегда осталась бы нулевой.
// База без сохранённого boot_id при известном boot_id считается пережившей перезапуск (так бывает при переходе на версию с boot_id).
// Чистая функция, покрыта тестом.
// Ledger перезапущен с момента базы: остатки бирж в нём обнулились, и база не годится. Известный boot_id при базе без него тоже считается сменой.
// Неизвестный boot_id (пустая строка — старый ledger) перезапуском не считается. Один критерий для семплера и для маршрута.
function ledgerRestarted(base, bootId) {
  return !!base && !!bootId && base.boot_id !== bootId;
}

function decideBaseline({ base, resetMs, q, m, bootId }) {
  const sameReset = !!base && Number(base.reset_ms) === Number(resetMs);
  const restarted = sameReset && ledgerRestarted(base, bootId);
  if (sameReset && !restarted) return 'keep';
  const ccys = Object.keys(q || {});
  if (ccys.length === 0) return 'wait';
  for (const c of ccys) {
    if (c === 'USDT' || !(Number(q[c]) !== 0) || !finite(q[c])) continue;
    if (!finite(m && m[c]) || !(Number(m[c]) > 0)) return 'wait';
  }
  return restarted ? 'rebase' : 'create';
}

// q, q0 — остатки по валютам {ccy: число}; m, m0 — цены в USDT на сейчас и на момент базы. Валюта без текущей цены, у которой есть
// остаток, в итог не входит и попадает в skipped (честно «не оценено», а не ноль). Нет цены базы — берётся текущая (переоценка 0).
// По валютам (by_currency) считаются от текущей базы; итоговые trading/revaluation включают перенос carry.
function computeSystemPnl({ q, q0, m, m0, carry }) {
  const ccys = new Set([...Object.keys(q || {}), ...Object.keys(q0 || {})]);
  const byCurrency = [];
  const skipped = [];
  let trading = 0, reval = 0, mtm = 0, mtm0 = 0;
  for (const c of [...ccys].sort()) {
    const qc = finite(q && q[c]) ? Number(q[c]) : 0;
    const q0c = finite(q0 && q0[c]) ? Number(q0[c]) : 0;
    const mc = c === 'USDT' ? 1 : (finite(m && m[c]) ? Number(m[c]) : null);
    if (mc === null) {
      if (qc !== 0 || q0c !== 0) skipped.push(c);
      continue;
    }
    const m0c = c === 'USDT' ? 1 : (finite(m0 && m0[c]) ? Number(m0[c]) : mc);
    const t = (qc - q0c) * mc;
    const v = q0c * (mc - m0c);
    trading += t; reval += v; mtm += qc * mc; mtm0 += q0c * m0c;
    byCurrency.push({ currency: c, q0: q0c, q: qc, dq: qc - q0c, m0: m0c, m: mc, trading: t, revaluation: v });
  }
  // carry — накопленные значения до перезапуска ledger (база была перенесена): итоги = перенос + изменение с новой базы
  const cT = carry && finite(carry.trading) ? Number(carry.trading) : 0;
  const cV = carry && finite(carry.revaluation) ? Number(carry.revaluation) : 0;
  return {
    trading: trading + cT, revaluation: reval + cV, mtm_change: trading + reval + cT + cV, mtm, mtm0,
    carry: { trading: cT, revaluation: cV }, by_currency: byCurrency, skipped,
  };
}

module.exports = { assetUsdMids, computeSystemPnl, decideBaseline, ledgerRestarted };
