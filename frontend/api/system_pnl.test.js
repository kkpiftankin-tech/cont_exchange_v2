// Self-contained node-тест: `node system_pnl.test.js` (у frontend/api нет jest).
const { assetUsdMids, computeSystemPnl, decideBaseline, ledgerRestarted } = require('./system_pnl');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}
const near = (a, b, eps = 1e-6) => Math.abs(a - b) < eps;

// --- пример из карточки: база 10 BTC и 1 000 000 USDT при BTC = 80 000; куплен 1 BTC по 80 100; рынок ушёл на 80 300
const q0 = { BTC: 10, USDT: 1000000 };
const m0 = { BTC: 80000 };
const q = { BTC: 11, USDT: 1000000 - 80100 };
const m = { BTC: 80300 };
let r = computeSystemPnl({ q, q0, m, m0 });
check('торговый результат = 1·80300 − 80100 = +200', near(r.trading, 200));
check('переоценка запасов = 10·(80300 − 80000) = +3000', near(r.revaluation, 3000));
check('изменение стоимости остатков = +3200', near(r.mtm_change, 3200));
check('тождество T + V = Σq·m − Σq0·m0', near(r.trading + r.revaluation, r.mtm - r.mtm0));
check('стоимость остатков до и после', near(r.mtm0, 1800000) && near(r.mtm, 1803200));
check('по валютам: BTC Δq=1, USDT Δq=−80100', r.by_currency.find((x) => x.currency === 'BTC').dq === 1 && near(r.by_currency.find((x) => x.currency === 'USDT').dq, -80100));

// --- рынок двигается, сделок нет: торговый результат 0, вся разница — переоценка (чего не различала прежняя оценка)
r = computeSystemPnl({ q: q0, q0, m: { BTC: 81000 }, m0 });
check('без сделок торговый результат равен нулю', near(r.trading, 0));
check('без сделок вся разница — переоценка 10·1000 = 10000', near(r.revaluation, 10000) && near(r.mtm_change, 10000));

// --- цена не менялась: переоценка 0, результат только от сделок
r = computeSystemPnl({ q, q0, m: m0, m0 });
check('цена не менялась: переоценка 0, торговый результат = −80100 + 1·80000 = −100', near(r.revaluation, 0) && near(r.trading, -100));

// --- валюта без цены не входит в итог и попадает в skipped (не в ноль)
r = computeSystemPnl({ q: { BTC: 11, SOL: 5, USDT: 100 }, q0: { BTC: 10, SOL: 5, USDT: 100 }, m: { BTC: 80000 }, m0: { BTC: 80000 } });
check('SOL без цены: skipped, в итог не входит', r.skipped.join() === 'SOL' && !r.by_currency.some((x) => x.currency === 'SOL'));
check('нулевой остаток без цены не считается пропуском', computeSystemPnl({ q: { XRP: 0, USDT: 1 }, q0: { XRP: 0, USDT: 1 }, m: {}, m0: {} }).skipped.length === 0);

// --- нет цены базы: берётся текущая (переоценка 0), валюта появилась после базы (q0 нет): q0 = 0
r = computeSystemPnl({ q: { BTC: 2, ETH: 3, USDT: 0 }, q0: { BTC: 2, USDT: 0 }, m: { BTC: 90000, ETH: 2400 }, m0: {} });
check('нет цены базы ⇒ переоценка 0', near(r.revaluation, 0));
check('валюта без базы: q0 = 0, весь остаток — торговый результат 3·2400', near(r.trading, 7200));

// --- мусор
r = computeSystemPnl({ q: null, q0: undefined, m: null, m0: null });
check('пустые входы не падают', r.trading === 0 && r.by_currency.length === 0);
r = computeSystemPnl({ q: { BTC: 'x', USDT: 5 }, q0: { BTC: 1, USDT: 5 }, m: { BTC: 'y' }, m0: {} });
check('нечисловые значения не дают NaN', Number.isFinite(r.trading) && r.skipped.includes('BTC'));

// --- цены актива: среднее по биржам пары ASSET/USDT
const mids = assetUsdMids([
  { venue_id: 'binance', pair: 'BTC/USDT', mid: '80000' }, { venue_id: 'okx', pair: 'BTC/USDT', mid: '80100' },
  { venue_id: 'kraken', pair: 'ETH/USDT', mid: '2400' }, { venue_id: 'binance', pair: 'ETH/BTC', mid: '0.03' },
  { venue_id: 'okx', pair: 'SOL/USDT', mid: '0' }, { venue_id: 'okx', pair: 'XRP/USDT', mid: 'x' }, null,
], ['BTC', 'ETH', 'SOL', 'USDT', 'XRP']);
check('BTC: медиана двух бирж 80050', near(mids.BTC, 80050));
check('ETH: единственная биржа; кросс-пара ETH/BTC игнорируется', near(mids.ETH, 2400));
check('USDT = 1; SOL (нулевая цена) и XRP (мусор) без цены', mids.USDT === 1 && !('SOL' in mids) && !('XRP' in mids));

// --- медиана и свежесть: тонкая биржа с выбросом не двигает цену, устаревший сегмент не используется
const now = 1000000;
const mids2 = assetUsdMids([
  { venue_id: 'binance', pair: 'BTC/USDT', mid: '80000', event_time_ms: now - 5000 },
  { venue_id: 'okx', pair: 'BTC/USDT', mid: '80040', event_time_ms: now - 8000 },
  { venue_id: 'coinbase', pair: 'BTC/USDT', mid: '79000', event_time_ms: now - 400000 },   // старый сегмент
  { venue_id: 'kraken', pair: 'BTC/USDT', mid: '90000', event_time_ms: now - 2000 },        // выброс: медиана из трёх свежих
  { venue_id: 'okx', pair: 'ETH/USDT', mid: '2400', event_time_ms: now - 500000 },          // единственный сегмент ETH устарел
], ['BTC', 'ETH', 'USDT'], now);
check('свежесть: старый сегмент Coinbase не учитывается; медиана 80040 устойчива к выбросу Kraken', near(mids2.BTC, 80040));
check('свежесть: единственный устаревший сегмент ⇒ цены нет (валюта будет «без цены»)', !('ETH' in mids2));
check('без nowMs фильтра свежести нет', 'ETH' in assetUsdMids([{ venue_id: 'okx', pair: 'ETH/USDT', mid: '2400', event_time_ms: 1 }], ['ETH']));

// --- решение о базе
const base = { reset_ms: 5, q0: {}, m0: {} };
check('база есть и сброс тот же ⇒ keep', decideBaseline({ base, resetMs: 5, q: { BTC: 1 }, m: {} }) === 'keep');
check('нет базы, цены всех валют с остатком есть ⇒ create', decideBaseline({ base: null, resetMs: 5, q: { BTC: 1, USDT: 100 }, m: { BTC: 80000 } }) === 'create');
check('сброс статистики (метка другая) ⇒ create заново', decideBaseline({ base, resetMs: 6, q: { BTC: 1 }, m: { BTC: 80000 } }) === 'create');
check('нет цены у валюты с остатком ⇒ wait (иначе переоценка навсегда 0)', decideBaseline({ base: null, resetMs: 5, q: { BTC: 1, SOL: 7 }, m: { BTC: 80000 } }) === 'wait');
check('валюта с нулевым остатком без цены не мешает', decideBaseline({ base: null, resetMs: 5, q: { BTC: 1, SOL: 0 }, m: { BTC: 80000 } }) === 'create');
check('нет остатков вообще ⇒ wait', decideBaseline({ base: null, resetMs: 5, q: {}, m: {} }) === 'wait');
check('база от прошлого сброса при отсутствии цен ⇒ wait, а не keep', decideBaseline({ base, resetMs: 9, q: { BTC: 1 }, m: {} }) === 'wait');

// --- перезапуск ledger (boot_id)
const baseB = { reset_ms: 5, boot_id: 'boot-1', q0: {}, m0: {} };
check('тот же запуск ledger ⇒ keep', decideBaseline({ base: baseB, resetMs: 5, q: { BTC: 1 }, m: { BTC: 80000 }, bootId: 'boot-1' }) === 'keep');
check('сменился boot_id ⇒ rebase', decideBaseline({ base: baseB, resetMs: 5, q: { BTC: 1 }, m: { BTC: 80000 }, bootId: 'boot-2' }) === 'rebase');
check('база без boot_id при известном boot_id ⇒ rebase (переход на версию с boot_id)', decideBaseline({ base: { reset_ms: 5, q0: {}, m0: {} }, resetMs: 5, q: { BTC: 1 }, m: { BTC: 80000 }, bootId: 'boot-2' }) === 'rebase');
check('boot_id неизвестен (старый ledger) ⇒ keep', decideBaseline({ base: baseB, resetMs: 5, q: { BTC: 1 }, m: { BTC: 80000 }, bootId: '' }) === 'keep');
check('rebase при отсутствии цены ⇒ wait', decideBaseline({ base: baseB, resetMs: 5, q: { BTC: 1 }, m: {}, bootId: 'boot-2' }) === 'wait');
check('сброс статистики важнее перезапуска ⇒ create (накопленное обнуляется)', decideBaseline({ base: baseB, resetMs: 6, q: { BTC: 1 }, m: { BTC: 80000 }, bootId: 'boot-2' }) === 'create');

// --- критерий перезапуска общий для семплера и маршрута
check('ledgerRestarted: тот же boot_id — нет', ledgerRestarted({ boot_id: 'a' }, 'a') === false);
check('ledgerRestarted: другой boot_id — да', ledgerRestarted({ boot_id: 'a' }, 'b') === true);
check('ledgerRestarted: база без boot_id, ответ с boot_id — да', ledgerRestarted({}, 'b') === true && ledgerRestarted({ boot_id: '' }, 'b') === true);
check('ledgerRestarted: boot_id ответа неизвестен — нет', ledgerRestarted({ boot_id: 'a' }, '') === false && ledgerRestarted({ boot_id: 'a' }, undefined) === false);
check('ledgerRestarted: базы нет — нет', ledgerRestarted(null, 'b') === false);

// --- перенос накопленного (carry): итог = перенос + изменение с новой базы
r = computeSystemPnl({ q: { BTC: 11, USDT: 919900 }, q0: { BTC: 10, USDT: 1000000 }, m: { BTC: 80300 }, m0: { BTC: 80000 }, carry: { trading: 50, revaluation: 20 } });
check('carry: торговый результат 200 + 50, переоценка 3000 + 20', near(r.trading, 250) && near(r.revaluation, 3020) && near(r.mtm_change, 3270));
check('carry: по валютам считается от новой базы', near(r.by_currency.find((x) => x.currency === 'BTC').trading, 80300));
check('carry не задан ⇒ нулевой перенос', computeSystemPnl({ q, q0, m, m0 }).carry.trading === 0);

if (failed) { console.error(failed + ' check(s) failed'); process.exit(1); }
console.log('all passed');
