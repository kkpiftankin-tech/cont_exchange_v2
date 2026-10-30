// Self-contained node-тест: `node agent_placeholders.test.js`. Недостающие агенты панели «Живые позиции».
const assert = require('assert');
const { placeholderAgentRows } = require('./agent_placeholders');
let failed = 0;
const check = (n, c) => { if (c) console.log('[OK] ' + n); else { console.error('[FAIL] ' + n); failed++; } };

const edges = [
  { name: 'T_BTC_binance', kind: 'translator', asset: 'BTC', quote: 'USDT', venue: 'binance' },
  { name: 'T_ETH_BTC_okx', kind: 'translator', asset: 'ETH', quote: 'BTC', venue: 'okx' },
  { name: 'A_BTC_binanceokx', kind: 'arbitrageur', asset: 'BTC', src: 'binance', dst: 'okx' },
  { name: 'A_USDT_binanceokx', kind: 'arbitrageur', asset: 'USDT', src: 'binance', dst: 'okx' },
];

{
  const rows = placeholderAgentRows(edges, new Set(['T_BTC_binance']));
  check('уже показанный агент не дублируется', !rows.some((r) => r.agent_id === 'T_BTC_binance') && rows.length === 3);
  const t = rows.find((r) => r.agent_id === 'T_ETH_BTC_okx');
  check('переводчик: пара, биржа, нулевые ноги и позиция', t.pair === 'ETH/BTC' && t.venue === 'okx' && t.pos_base === 0 && t.pos_quote === 0 && t.c_total === 0 && t.inactive === true && t.hasDelta === false);
  const a = rows.find((r) => r.agent_id === 'A_BTC_binanceokx');
  check('арбитражёр: маршрут, нулевые ноги, базис неизвестен', a.arb.src === 'binance' && a.arb.dst === 'okx' && a.arb.leg_src === 0 && a.arb.leg_dst === 0 && a.pair === 'BTC/USDT' && a.basis_mtm_usdt === null);
  const u = rows.find((r) => r.agent_id === 'A_USDT_binanceokx');
  check('арбитражёр нумерария: пара USDT, без котируемой', u.pair === 'USDT' && u.quote === '');
}
check('нет рёбер — нет строк', placeholderAgentRows([], new Set()).length === 0 && placeholderAgentRows(undefined, undefined).length === 0 && placeholderAgentRows(null, null).length === 0);
check('мусорные рёбра пропускаются', placeholderAgentRows([{}, null, { name: 'X' }, { name: 'T_x', kind: 'translator' }], new Set()).length === 0);
check('повторное ребро с тем же именем даёт одну строку', placeholderAgentRows([edges[0], edges[0]], new Set()).length === 1);

assert.strictEqual(failed, 0, failed + ' failed');
console.log('agent_placeholders tests passed');
