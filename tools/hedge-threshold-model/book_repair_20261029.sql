-- ОДНОРАЗОВО (инцидент 2026-10-29 12:10 UTC, см. docs/00-methodology/known-gaps.md). ПРИМЕНЁН 2026-10-29 14:26-14:27 UTC по разрешению владельца стенда.
-- Порядок: остановить ledger (docker stop infra-ledger-1) -> выполнить этот файл, дописав в конец COMMIT; (без COMMIT транзакция откатится) ->
--   поднять ledger (docker compose -f docker-compose.dev.yml up -d --no-build --no-deps --force-recreate ledger).
-- Копия таблицы до правки: ce_agent_pnl_book_bak_20261029. Откат: UPDATE ... FROM копии по agent_id.
-- Запуск: psql -f book_repair_20261029.sql (внутри контейнера postgres, БД и пользователь из его окружения).
-- Восстановление внешнего реализованного результата книг после искажения 2026-10-29 12:10 UTC (переполнение Decimal::div в книге агента).
-- Берём последнюю чистую минутную запись истории ДО 12:10:00 UTC. Выполнять при остановленном ledger (иначе он перезапишет строки).
\set ON_ERROR_STOP on
BEGIN;
CREATE TABLE IF NOT EXISTS ce_agent_pnl_book_bak_20261029 AS SELECT now() AS bak_at, * FROM ce_agent_pnl_book;
SELECT 'до' AS what, agent_id, qty, avg_fair, edge, ext_realized FROM ce_agent_pnl_book
 WHERE agent_id IN ('T_BTC_binance', 'T_BTC_kraken', 'T_BTC_okx', 'T_ETH_binance', 'T_ETH_kraken') ORDER BY agent_id;
UPDATE ce_agent_pnl_book b
   SET ext_realized = h.realized_usdt, updated_at = now()
  FROM (SELECT DISTINCT ON (agent_id) agent_id, realized_usdt
          FROM ce_agent_pnl_history
         WHERE ts < '2026-10-29 12:10:00+00' AND realized_usdt IS NOT NULL
           AND agent_id IN ('T_BTC_binance', 'T_BTC_kraken', 'T_BTC_okx', 'T_ETH_binance', 'T_ETH_kraken')
         ORDER BY agent_id, ts DESC) h
 WHERE b.agent_id = h.agent_id;
SELECT 'после' AS what, agent_id, qty, avg_fair, edge, ext_realized FROM ce_agent_pnl_book
 WHERE agent_id IN ('T_BTC_binance', 'T_BTC_kraken', 'T_BTC_okx', 'T_ETH_binance', 'T_ETH_kraken') ORDER BY agent_id;
