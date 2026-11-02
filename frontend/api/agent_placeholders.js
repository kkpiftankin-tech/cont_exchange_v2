'use strict';
// Панель «Живые позиции»: показывать ВСЕХ агентов клиринга, в том числе тех, у кого в ledger ещё нет строки позиции
// (не действовали). Список агентов берётся из последнего решения клиринга (matching, /ce/clearing/last: рёбра графа =
// переводчики и арбитражёры), недостающие добавляются нулевыми строками той же формы, что и строки buildAgentRows.
// Чистая функция: данные и пояснения — как есть, ничего не пересчитывается.

const num0 = () => 0;

// edges — рёбра снимка клиринга ([{name, kind: 'translator'|'arbitrageur', asset, quote, venue, src, dst}]);
// existingIds — agent_id, которые в панели уже есть. Возвращает только недостающие строки.
function placeholderAgentRows(edges, existingIds) {
  const have = existingIds instanceof Set ? existingIds : new Set(existingIds || []);
  const out = [];
  for (const e of Array.isArray(edges) ? edges : []) {
    const id = e && e.name;
    if (!id || have.has(id)) continue;
    if (e.kind === 'translator' && e.asset && e.venue) {
      const quote = e.quote || 'USDT';
      out.push({
        agent_id: id, agent_kind: 'translator', pair: `${e.asset}/${quote}`, base: e.asset, quote, venue: e.venue,
        hasDelta: false, inactive: true,
        pos_base_before: num0(), delta_base: num0(), pos_base: num0(),
        pos_quote_before: num0(), delta_quote: num0(), pos_quote: num0(),
        c_position: num0(), c_total: num0(), in_flight: num0(), c_delta: num0(), pair_price: null, arb: null,
      });
    } else if (e.kind === 'arbitrageur' && e.asset && e.src && e.dst) {
      out.push({
        agent_id: id, agent_kind: 'arbitrageur', pair: e.asset === 'USDT' ? 'USDT' : `${e.asset}/USDT`, base: e.asset,
        quote: e.asset === 'USDT' ? '' : 'USDT', venue: '', hasDelta: false, inactive: true, pair_price: null,
        arb: { src: e.src, dst: e.dst, unit: e.asset, leg_src: num0(), leg_dst: num0(), bleg_src: num0(), bleg_dst: num0(), dleg_src: num0(), dleg_dst: num0() },
        basis_mtm_usdt: null,
      });
    }
    have.add(id);
  }
  return out;
}

module.exports = { placeholderAgentRows };
