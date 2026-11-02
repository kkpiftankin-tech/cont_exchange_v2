#!/bin/bash
# Абсолютный сброс контура CE на dev-хосте (ADR-090, CR-F18-018, этап Э1): возвращает песочницу в исходное состояние эксперимента и
# создаёт метку (эпоху) kind='absolute'. Кодифицирует ручной «чистый старт» (20:35 UTC 2026-10-29): остановить ledger → дождаться пустых групп
# Kafka → перемотать их в конец → обнулить состояние в Postgres ОДНОЙ транзакцией вместе с меткой и аудитом → запустить ledger. Перезапуск ledger
# даёт остальное исходное состояние (остатки площадок, план по активу, очередь переводов, оценки потока, пороги агентов — только память процесса).
# Метка открывается ДО запуска ledger, поэтому вся активность нового запуска принадлежит новой эпохе. Этап Э2 заменит скрипт операцией ledger.
#
# ВНИМАНИЕ: необратимо для текущего состояния агентов (срез до сброса сохраняется в каталоге копий). Только песочница.
# По умолчанию — РЕЖИМ ПЛАНА (dry-run): ничего не меняется, печатаются проверки и план. Для выполнения нужны --yes и CE_RESET_ALLOWED=1.
#
# Запуск на dev-хосте (nik@ubuntu-dev):
#   CE_RESET_ALLOWED=1 tools/reset/absolute_reset.sh --note "название эксперимента" [--actor имя] [--request-id ID] [--yes]
set -euo pipefail

NOTE=""; ACTOR="${USER:-operator}"; REQUEST_ID=""; YES=0
INFRA_DIR="${INFRA_DIR:-/home/nik/cont_exchange_v2/infra}"
BACKUP_ROOT="${BACKUP_ROOT:-/home/nik/reset-backups}"
GROUPS_TO_SEEK=(ledger-ce-pos-delta ledger-exec ledger-intents)
PG_CONTAINER="${PG_CONTAINER:-infra-postgres-1}"
RP_CONTAINER="${RP_CONTAINER:-infra-redpanda-1}"
LEDGER_CONTAINER="${LEDGER_CONTAINER:-infra-ledger-1}"

while [ $# -gt 0 ]; do
  case "$1" in
    --note) NOTE="$2"; shift 2;;
    --actor) ACTOR="$2"; shift 2;;
    --request-id) REQUEST_ID="$2"; shift 2;;
    --yes) YES=1; shift;;
    -h|--help) sed -n '2,15p' "$0"; exit 0;;
    *) echo "неизвестный параметр: $1" >&2; exit 2;;
  esac
done

die() { echo "ОТКАЗ: $*" >&2; exit 1; }
pg() { docker exec -i "$PG_CONTAINER" psql -U cex -d cex -v ON_ERROR_STOP=1 -q -t -A "$@"; }

# ---- проверки до любых изменений
[ -n "$NOTE" ] || die "укажите название эксперимента: --note \"...\" (1-200 знаков)"
NOTE_LEN=$(printf '%s' "$NOTE" | wc -m | tr -d ' ')
{ [ "$NOTE_LEN" -ge 1 ] && [ "$NOTE_LEN" -le 200 ]; } || die "название эксперимента должно быть 1-200 знаков (сейчас $NOTE_LEN)"
case "$NOTE$ACTOR" in *$'\n'*|*$'\r'*|*$'\t'*) die "управляющие символы в --note/--actor не допускаются";; esac
[ -n "$REQUEST_ID" ] || REQUEST_ID="abs-$(date -u +%Y%m%dT%H%M%S)-$RANDOM"
printf '%s' "$REQUEST_ID" | grep -Eq '^[A-Za-z0-9_-]{8,80}$' || die "request-id: 8-80 знаков [A-Za-z0-9_-]"
for c in "$PG_CONTAINER" "$RP_CONTAINER" "$LEDGER_CONTAINER"; do
  docker inspect -f '{{.State.Running}}' "$c" 2>/dev/null | grep -q true || die "контейнер $c не запущен (скрипт запускают на dev-хосте)"
done
pg -c "SELECT 1 FROM pg_proc WHERE proname = 'ce_epoch_open'" | grep -q 1 \
  || die "в Postgres нет схемы меток (функции ce_epoch_open): запустите BFF версии Э1 — он создаёт схему при первом обращении"
TOO_RECENT=$(pg -c "SELECT COALESCE(clock_timestamp() - max(reset_at) < interval '60 seconds', false) FROM ce_reset_epoch")
[ "$TOO_RECENT" = "f" ] || die "последняя метка создана менее минуты назад (повторный сброс подряд не допускается)"
CUR_EPOCH=$(pg -c "SELECT ce_current_epoch()")

# параметры исходного состояния: сиды капитала из окружения ledger (то, что получит ledger при запуске)
SEEDS=$(docker exec "$LEDGER_CONTAINER" printenv | grep '^CE_CAPITAL_SEED_' | sort | tr '\n' ' ' || true)
CONFIG_VERSION=$(pg -c "SELECT COALESCE(config_version::text, '') FROM f05a_clearing_config WHERE id = 1" 2>/dev/null || true)

echo "== абсолютный сброс контура CE =="
echo "   название : $NOTE"
echo "   автор    : $ACTOR   request_id: $REQUEST_ID"
echo "   текущая эпоха: $CUR_EPOCH   сиды ledger: ${SEEDS:-не найдены}"
echo "   группы Kafka к перемотке в конец: ${GROUPS_TO_SEEK[*]}"
echo "   план: срез PG → stop ledger → ждать пустые группы → seek → [одна транзакция PG: метка + обнуление + аудит] → start ledger"

if [ "$YES" -ne 1 ] || [ "${CE_RESET_ALLOWED:-0}" != "1" ]; then
  echo
  echo "РЕЖИМ ПЛАНА: ничего не изменено. Для выполнения: CE_RESET_ALLOWED=1 $0 --note \"...\" --yes   (только песочница)"
  exit 0
fi

# ---- срез до сброса (для аудита и разбора)
TS="$(date -u +%Y%m%dT%H%M%SZ)"
BK="$BACKUP_ROOT/$TS"
mkdir -p "$BK"
pg -c "\\copy (SELECT * FROM ce_agent_position ORDER BY 1,2,3) TO STDOUT WITH CSV HEADER" > "$BK/ce_agent_position.csv"
pg -c "\\copy (SELECT * FROM ce_agent_pnl_book) TO STDOUT WITH CSV HEADER" > "$BK/ce_agent_pnl_book.csv" 2>/dev/null || true
BEFORE_JSON=$(pg -c "SELECT jsonb_build_object('source','absolute_reset.sh','backup_dir','$BK','agents',count(DISTINCT agent_id),'rows',count(*),
   'sum_abs_position_kusdt',COALESCE(sum(abs(position)),0)::text,'sum_abs_in_flight_kusdt',COALESCE(sum(abs(in_flight)),0)::text,
   'sum_abs_realized_vs_fair',COALESCE(sum(abs(realized_vs_fair)),0)::text)::text FROM ce_agent_position")
INITIAL_JSON=$(printf '{"source":"absolute_reset.sh","seeds":"%s","note":"исходное состояние = холодный запуск контура CE"}' "$(printf '%s' "$SEEDS" | sed 's/"/\\"/g')")
echo "   срез сохранён: $BK"

LEDGER_STOPPED=0
restore_ledger() {
  if [ "$LEDGER_STOPPED" -eq 1 ]; then
    echo "!! сбой: возвращаю ledger в работу (состояние PG не менялось, если транзакция не прошла)" >&2
    (cd "$INFRA_DIR" && docker compose -f docker-compose.dev.yml up -d --no-deps ledger) >&2 || echo "!! ledger не запустился — запустите вручную" >&2
  fi
}
trap restore_ledger ERR

# ---- остановить ledger и дождаться пустых групп
(cd "$INFRA_DIR" && docker compose -f docker-compose.dev.yml stop ledger)
LEDGER_STOPPED=1
for g in "${GROUPS_TO_SEEK[@]}"; do
  ok=0
  for _ in $(seq 1 60); do
    st=$(docker exec "$RP_CONTAINER" rpk group describe "$g" 2>/dev/null | awk '/^STATE/{print $2}')
    if [ "$st" = "Empty" ] || [ -z "$st" ]; then ok=1; break; fi
    sleep 2
  done
  [ "$ok" -eq 1 ] || die "группа $g не опустела за 120 с"
  docker exec "$RP_CONTAINER" rpk group seek "$g" --to end
done

# ---- одна транзакция: метка + обнуление состояния + запасная метка + аудит
pg -v actor="$ACTOR" -v note="$NOTE" -v rid="$REQUEST_ID" -v before="$BEFORE_JSON" -v initial="$INITIAL_JSON" -v cfg="$CONFIG_VERSION" <<'SQL'
BEGIN;
SET LOCAL lock_timeout = '5s';
SELECT epoch_id AS new_epoch, to_char(reset_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"') AS new_reset_at
  FROM ce_epoch_open(:'actor', 'absolute', :'note', :'rid', :'before'::jsonb, :'initial'::jsonb, NULL, NULLIF(:'cfg', '')::bigint) \gset
UPDATE ce_agent_position SET position = 0, in_flight = 0, realized_vs_fair = 0;
DELETE FROM ce_agent_pnl_book;
UPDATE f05a_clearing_config SET trade_stats_reset_at = :'new_reset_at'::timestamptz WHERE id = 1;
DO $$ BEGIN IF to_regclass('ce_system_pnl_baseline') IS NOT NULL THEN DELETE FROM ce_system_pnl_baseline; END IF; END $$;
INSERT INTO operator_audit_log (actor, action, remote_addr, user_agent, details)
  VALUES (:'actor', 'ce_reset_absolute', 'dev-host', 'tools/reset/absolute_reset.sh',
          jsonb_build_object('msg', 'ADR-090 абсолютный сброс контура CE', 'epoch', :new_epoch, 'kind', 'absolute', 'note', :'note', 'request_id', :'rid', 'before', :'before'::jsonb));
COMMIT;
SELECT 'эпоха ' || :new_epoch || ' создана ' || :'new_reset_at';
SQL

# ---- запустить ledger (в памяти — исходное состояние: остатки площадок, план, очереди, пороги)
(cd "$INFRA_DIR" && docker compose -f docker-compose.dev.yml up -d --no-deps ledger)
LEDGER_STOPPED=0
trap - ERR
echo "== готово: ledger запущен. Проверьте экран «Клиринг»: позиции с нуля, в «Баланс» план по активу = 0, эпоха — GET /api/vector-clearing/epochs"
echo "   Оценки потока модели ADR-088 начнутся заново (около часа пороги шире); режим модели при необходимости переключите в shadow."
