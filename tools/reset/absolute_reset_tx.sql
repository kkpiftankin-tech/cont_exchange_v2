-- Транзакция абсолютного сброса контура CE (ADR-090, Э1): метка + обнуление состояния + запасная метка + аудит — всё или ничего.
-- Запускается tools/reset/absolute_reset.sh: сначала как ПРОБНЫЙ прогон с откатом (до остановки ledger), затем по-настоящему (после остановки ledger и
-- перемотки групп Kafka). Тот же файл прогоняет tools/reset/test_absolute_reset_sql.sh в откатываемой транзакции во временной схеме.
-- Параметры psql (-v): actor, note, rid, backup_dir, seeds, os_user, sudo_user, ssh_conn, host, script_sha, sql_sha, remote_addr.
-- Строка `COMMIT;` ниже должна встречаться в файле ровно один раз (скрипт подменяет её на откат в пробном прогоне и проверяет это).
BEGIN;
SET LOCAL lock_timeout = '5s';
-- снимок состояния ДО сброса — внутри транзакции, поэтому он согласован с тем, что именно обнуляется
SELECT jsonb_build_object('source', 'absolute_reset.sh', 'backup_dir', :'backup_dir', 'agents', count(DISTINCT agent_id), 'rows', count(*),
         'sum_abs_position_kusdt', COALESCE(sum(abs(position)), 0)::text, 'sum_abs_in_flight_kusdt', COALESCE(sum(abs(in_flight)), 0)::text,
         'sum_abs_realized_vs_fair', COALESCE(sum(abs(realized_vs_fair)), 0)::text)::text AS before_json FROM ce_agent_position \gset
SELECT jsonb_build_object('source', 'absolute_reset.sh', 'seeds', :'seeds', 'note', 'исходное состояние = холодный запуск контура CE')::text AS initial_json \gset
SELECT count(*) AS n_before FROM ce_reset_epoch \gset
SELECT epoch_id AS new_epoch, to_char(reset_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"') AS new_reset_at
  FROM ce_epoch_open(:'actor', 'absolute', :'note', :'rid', :'before_json'::jsonb, :'initial_json'::jsonb, NULL,
                     (SELECT config_version FROM f05a_clearing_config WHERE id = 1)) \gset
-- Повтор request_id: ce_epoch_open возвращает прежнюю эпоху и НЕ создаёт новую, а обнуление ниже всё равно выполнилось бы — «сброс без метки».
-- Метка обязана быть создана именно этой транзакцией: строк стало ровно на одну больше и она последняя (чужая вставка между шагами — тоже отказ).
SELECT (count(*) = :n_before + 1 AND :new_epoch = max(epoch_id)) AS fresh FROM ce_reset_epoch \gset
\if :fresh
\else
  DO $$ BEGIN RAISE EXCEPTION 'absolute_reset: эпоха не новая (повтор request_id или параллельная метка) — обнуление отменено'; END $$;
\endif
UPDATE ce_agent_position SET position = 0, in_flight = 0, realized_vs_fair = 0;
DELETE FROM ce_agent_pnl_book;
UPDATE f05a_clearing_config SET trade_stats_reset_at = :'new_reset_at'::timestamptz WHERE id = 1;
DO $$ BEGIN IF to_regclass('ce_system_pnl_baseline') IS NOT NULL THEN DELETE FROM ce_system_pnl_baseline; END IF; END $$;
INSERT INTO operator_audit_log (actor, action, remote_addr, user_agent, details)
  VALUES (:'actor', 'ce_reset_absolute', NULLIF(:'remote_addr', ''), 'tools/reset/absolute_reset.sh',
          jsonb_build_object('msg', 'ADR-090 абсолютный сброс контура CE', 'epoch', :new_epoch, 'kind', 'absolute', 'note', :'note', 'request_id', :'rid',
                             'os_user', :'os_user', 'sudo_user', :'sudo_user', 'ssh_connection', :'ssh_conn', 'host', :'host',
                             'script_sha256', :'script_sha', 'sql_sha256', :'sql_sha', 'backup_dir', :'backup_dir', 'before', :'before_json'::jsonb));
COMMIT;
SELECT 'эпоха ' || :new_epoch || ' создана ' || :'new_reset_at';
