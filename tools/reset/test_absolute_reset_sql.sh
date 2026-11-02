#!/bin/bash
# Проверка SQL-транзакции абсолютного сброса (absolute_reset_tx.sql) на настоящем Postgres БЕЗ изменения рабочей базы: тот же файл выполняется в одной
# транзакции во временной схеме epoch_check с фикстурами (позиции с ненулевыми значениями, книга, база прибыли системы, метка конфигурации), результат
# проверяется ДО отката, затем всё откатывается — ни таблиц, ни строк не остаётся. Два прогона:
#   1) положительный — название с кавычками и обратной косой чертой, все побочные эффекты и аудит;
#   2) повтор request_id — транзакция обязана отказать («эпоха не новая»), а не обнулить состояние без метки.
# Запуск на машине с node и ssh-доступом к dev:  tools/reset/test_absolute_reset_sql.sh [ssh-хост] [контейнер-postgres]
set -euo pipefail
HOST="${1:-nik@ubuntu-dev}"
PGC="${2:-infra-postgres-1}"
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
SQL="$DIR/absolute_reset_tx.sql"

[ "$(grep -c '^COMMIT;$' "$SQL")" = "1" ] || { echo "ОШИБКА: в $SQL строка COMMIT; должна быть ровно одна" >&2; exit 1; }
PRE="$(sed '/^COMMIT;$/,$d' "$SQL")"      # всё до COMMIT
POST="$(sed '1,/^COMMIT;$/d' "$SQL")"     # всё после COMMIT (итоговая строка)

prelude() {   # схема меток + фикстуры + параметры, как их передал бы скрипт через psql -v
  echo "BEGIN;"
  echo "CREATE SCHEMA epoch_check;"
  echo "SET LOCAL search_path = epoch_check, pg_catalog;"
  node "$ROOT/frontend/api/epoch_schema.js" --print-sql
  cat <<'SQL'
-- фикстуры: минимум колонок, которые трогает транзакция
CREATE TABLE ce_agent_position (agent_id text, asset text, venue text, position numeric, in_flight numeric, realized_vs_fair numeric, last_batch_id text);
CREATE TABLE ce_agent_pnl_book (agent_id text, avg_fair numeric);
CREATE TABLE f05a_clearing_config (id integer primary key, trade_stats_reset_at timestamptz, config_version bigint);
CREATE TABLE ce_system_pnl_baseline (id integer primary key, reset_ms bigint);
CREATE TABLE operator_audit_log (id bigserial primary key, ts timestamptz default now(), actor text, action text, remote_addr text, user_agent text, details jsonb);
INSERT INTO ce_agent_position VALUES ('T_BTC_binance','BTC','binance', 12.5, -3.0, 7.7, 'w|1'), ('A_BTC_binanceokx','BTC','okx', -4.0, 1.0, 0.5, 'w|2');
INSERT INTO ce_agent_pnl_book VALUES ('T_BTC_binance', 80000), ('A_BTC_binanceokx', 80100);
INSERT INTO f05a_clearing_config VALUES (1, '2026-11-02 07:02:22+00', 16);
INSERT INTO ce_system_pnl_baseline VALUES (1, 1793602942169);
\set actor tester
\set note 'опыт ''проверки'' "q" \\ back'
\set rid rid-test-0001
\set backup_dir /tmp/reset-backups/test
\set seeds 'CE_CAPITAL_SEED_BTC=10 CE_CAPITAL_SEED_USDT=1000000 '
\set os_user nik
\set sudo_user ''
\set ssh_conn '10.0.0.1 50000 10.0.0.2 22'
\set host devhost
\set script_sha aaaa
\set sql_sha bbbb
\set remote_addr 10.0.0.1
SQL
}

checks() {
  cat <<'SQL'
DO $chk$ BEGIN
  IF (SELECT count(*) FROM ce_reset_epoch WHERE kind = 'absolute' AND request_id = 'rid-test-0001' AND actor = 'tester') <> 1 THEN RAISE EXCEPTION 'метка absolute не создана'; END IF;
  IF (SELECT note FROM ce_reset_epoch WHERE request_id = 'rid-test-0001') <> 'опыт ''проверки'' "q" \ back' THEN RAISE EXCEPTION 'название с кавычками/обратной чертой исказилось'; END IF;
  IF (SELECT before_state->>'agents' FROM ce_reset_epoch WHERE request_id = 'rid-test-0001') <> '2' THEN RAISE EXCEPTION 'снимок до сброса не записан'; END IF;
  IF (SELECT before_state->>'sum_abs_position_kusdt' FROM ce_reset_epoch WHERE request_id = 'rid-test-0001') <> '16.5' THEN RAISE EXCEPTION 'снимок до сброса: сумма позиций неверна'; END IF;
  IF (SELECT initial_state->>'seeds' FROM ce_reset_epoch WHERE request_id = 'rid-test-0001') NOT LIKE 'CE_CAPITAL_SEED_BTC=10%' THEN RAISE EXCEPTION 'сиды не записаны в исходное состояние'; END IF;
  IF (SELECT count(*) FROM ce_agent_position WHERE position <> 0 OR in_flight <> 0 OR realized_vs_fair <> 0) <> 0 THEN RAISE EXCEPTION 'позиции не обнулены'; END IF;
  IF (SELECT count(*) FROM ce_agent_position WHERE last_batch_id IS NULL) <> 0 THEN RAISE EXCEPTION 'last_batch_id (защита идемпотентности) затёрт'; END IF;
  IF (SELECT count(*) FROM ce_agent_position) <> 2 THEN RAISE EXCEPTION 'строки позиций удалены, а должны быть обнулены'; END IF;
  IF (SELECT count(*) FROM ce_agent_pnl_book) <> 0 THEN RAISE EXCEPTION 'книги не удалены'; END IF;
  IF (SELECT count(*) FROM ce_system_pnl_baseline) <> 0 THEN RAISE EXCEPTION 'база прибыли системы не удалена'; END IF;
  IF (SELECT trade_stats_reset_at FROM f05a_clearing_config WHERE id = 1) <> (SELECT reset_at FROM ce_reset_epoch WHERE request_id = 'rid-test-0001') THEN RAISE EXCEPTION 'запасная метка не равна моменту эпохи'; END IF;
  IF (SELECT count(*) FROM operator_audit_log WHERE action = 'ce_reset_absolute' AND actor = 'tester' AND (details->>'epoch')::bigint = ce_current_epoch()) <> 1 THEN RAISE EXCEPTION 'аудит не записан'; END IF;
  IF (SELECT details->>'os_user' || '|' || (details->>'sql_sha256') || '|' || (details->>'ssh_connection') || '|' || (details->>'host') FROM operator_audit_log WHERE action = 'ce_reset_absolute')
     <> 'nik|bbbb|10.0.0.1 50000 10.0.0.2 22|devhost' THEN RAISE EXCEPTION 'в аудите нет личности процесса (os_user/ssh/host/хеш)'; END IF;
  IF (SELECT remote_addr FROM operator_audit_log WHERE action = 'ce_reset_absolute') <> '10.0.0.1' THEN RAISE EXCEPTION 'в аудите нет адреса ssh-клиента'; END IF;
  IF (SELECT config_version FROM ce_reset_epoch WHERE request_id = 'rid-test-0001') <> 16 THEN RAISE EXCEPTION 'версия настроек не записана в метку'; END IF;
END $chk$;
SQL
}

run_psql() { ssh -o ConnectTimeout=10 "$HOST" "docker exec -i $PGC psql -U cex -d cex -v ON_ERROR_STOP=1 -q -t -A" 2>&1; }

# ---- 1) положительный прогон: тот же SQL, COMMIT заменён проверками результата и откатом
{ prelude; printf '%s\n' "$PRE"; checks; printf '%s\n' "$POST"; echo "SELECT 'absolute_reset_tx: положительный прогон — все проверки прошли';"; echo "ROLLBACK;"; } \
  | run_psql | grep -v "^WARNING:\|^NOTICE:\|^$"

# ---- 2) повтор request_id в той же сессии: второй проход обязан отказать, а не обнулить состояние без новой метки
set +e
OUT=$({ prelude; printf '%s\n' "$PRE"; echo "UPDATE ce_agent_position SET position = 5;"; printf '%s\n' "$PRE"; echo "ROLLBACK;"; } | run_psql)
RC=$?
set -e
if [ "$RC" -ne 0 ] && printf '%s' "$OUT" | grep -q "эпоха не новая"; then
  echo "absolute_reset_tx: повтор request_id отвергнут (обнуление отменено) — проверка прошла"
else
  echo "ОШИБКА: повтор request_id не отвергнут (rc=$RC):" >&2
  printf '%s\n' "$OUT" | tail -5 >&2
  exit 1
fi
