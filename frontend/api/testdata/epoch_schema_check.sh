#!/bin/bash
# Проверка схемы метки сброса на настоящем Postgres БЕЗ изменения рабочей базы: всё выполняется одной транзакцией во временной схеме
# epoch_check (search_path указывает только на неё) и в конце откатывается — ни таблиц, ни функций, ни строк не остаётся.
# Запуск на машине с node и ssh-доступом к dev:  frontend/api/testdata/epoch_schema_check.sh [ssh-хост] [контейнер-postgres]
# По умолчанию: nik@ubuntu-dev, infra-postgres-1.
set -euo pipefail
HOST="${1:-nik@ubuntu-dev}"
PGC="${2:-infra-postgres-1}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

{
  echo "BEGIN;"
  echo "CREATE SCHEMA epoch_check;"
  echo "SET LOCAL search_path = epoch_check, pg_catalog;"
  node "$DIR/epoch_schema.js" --print-sql   # схема
  node "$DIR/epoch_schema.js" --print-sql   # повторное применение не должно ничего ломать
  cat "$DIR/testdata/epoch_schema_check.sql"
  echo "ROLLBACK;"
} | ssh -o ConnectTimeout=10 "$HOST" "docker exec -i $PGC psql -U cex -d cex -v ON_ERROR_STOP=1 -q -t -A"
