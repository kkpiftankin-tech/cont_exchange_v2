#!/bin/bash
# Абсолютный сброс контура CE на dev-хосте (ADR-090, CR-F18-018, этап Э1): возвращает песочницу в исходное состояние эксперимента и создаёт метку
# (эпоху) kind='absolute'. Кодифицирует ручной «чистый старт» (20:35 UTC 2026-10-29): остановить ledger → дождаться пустых групп Kafka → снять срез →
# перемотать группы в конец → обнулить состояние в Postgres ОДНОЙ транзакцией вместе с меткой и аудитом (tools/reset/absolute_reset_tx.sql) → запустить
# ledger. Перезапуск ledger даёт остальное исходное состояние (остатки площадок, план по активу, очередь переводов, оценки потока, пороги агентов —
# только память процесса). Метка открывается ДО запуска ledger, поэтому вся активность нового запуска принадлежит новой эпохе.
#
# ВНИМАНИЕ: необратимо для текущего состояния агентов (срез до сброса сохраняется в каталоге копий). Только песочница: скрипт отказывается, если
# venues запущен без VENUES_SIMULATE_ORDERS=1. По умолчанию — РЕЖИМ ПЛАНА: проверки и пробная транзакция (откатывается), ничего не меняется.
# Для выполнения нужны --yes и CE_RESET_ALLOWED=1. Этап Э2 заменит скрипт операцией ledger.
#
# Если скрипт прерван (ошибка, Ctrl-C, обрыв SSH — после начала работы HUP игнорируется), он сам решает по этапу:
#   до перемотки Kafka  — состояние не менялось, ledger возвращается в работу автоматически;
#   после перемотки     — ledger НЕ запускается (пропущенные сообщения потеряны, Postgres не обнулён): печатаются инструкции, повторный запуск
#                         скрипта с теми же параметрами безопасен;
#   после COMMIT        — сброс выполнен; если ledger не поднялся, печатается команда запуска.
# Весь вывод дублируется в run.log каталога копии.
#
# Запуск на dev-хосте (nik@ubuntu-dev):
#   CE_RESET_ALLOWED=1 tools/reset/absolute_reset.sh --note "название эксперимента" [--actor имя] [--request-id ID] [--yes]
set -Eeuo pipefail
umask 077
export LC_ALL=C.UTF-8

NOTE=""; ACTOR="${SUDO_USER:-${USER:-operator}}"; REQUEST_ID=""; YES=0
INFRA_DIR="${INFRA_DIR:-/home/nik/cont_exchange_v2/infra}"
BACKUP_ROOT="${BACKUP_ROOT:-/home/nik/reset-backups}"
GROUPS_TO_SEEK=(ledger-ce-pos-delta ledger-exec ledger-intents)
PG_CONTAINER="infra-postgres-1"        # имена контейнеров намеренно не переопределяются окружением: проверка песочницы (venues) должна относиться к тем же стендам
RP_CONTAINER="infra-redpanda-1"
LEDGER_CONTAINER="infra-ledger-1"
VENUES_CONTAINER="infra-venues-1"
COMPOSE=(docker compose -f docker-compose.dev.yml)
MIN_FREE_KB=204800                       # место под срез: 200 МБ
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SQL_FILE="$SCRIPT_DIR/absolute_reset_tx.sql"

# этап нужен аварийному обработчику: что уже произошло и можно ли без риска вернуть ledger
#   preflight → stopping (ledger остановлен или останавливается; ни одного seek) → seeking (хотя бы один seek начат) → tx → committed → done
STAGE="preflight"; BK=""; NEW_EPOCH=""; PSQL_READY=""; AUDIT_DENIALS=0
PSQL_VARS=()

usage() { awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' "$0"; }
arg() { [ $# -ge 2 ] || { echo "параметр $1 требует значение" >&2; exit 2; }; }
while [ $# -gt 0 ]; do
  case "$1" in
    --note) arg "$@"; NOTE="$2"; shift 2;;
    --actor) arg "$@"; ACTOR="$2"; shift 2;;
    --request-id) arg "$@"; REQUEST_ID="$2"; shift 2;;
    --yes) YES=1; shift;;
    -h|--help) usage; exit 0;;
    *) echo "неизвестный параметр: $1" >&2; exit 2;;
  esac
done

die() { echo "ОТКАЗ: $*" >&2; DENY_MSG="$*"; exit 1; }
DENY_MSG=""
pg() { docker exec -e PGCONNECT_TIMEOUT=10 -e PGOPTIONS="-c statement_timeout=120000 -c lock_timeout=10000" -i "$PG_CONTAINER" psql -U cex -d cex -v ON_ERROR_STOP=1 -q -t -A "$@"; }

# запись в operator_audit_log (попытка, отказ, сбой); тело — heredoc, потому что psql не подставляет переменные в -c
audit_event() {   # $1 = действие, $2 = текст
  pg "${PSQL_VARS[@]}" -v "ev_action=$1" -v "ev_msg=$2" <<'SQL'
INSERT INTO operator_audit_log (actor, action, remote_addr, user_agent, details)
VALUES (:'actor', :'ev_action', NULLIF(:'remote_addr', ''), 'tools/reset/absolute_reset.sh',
        jsonb_build_object('msg', :'ev_msg', 'note', :'note', 'request_id', :'rid', 'os_user', :'os_user', 'sudo_user', :'sudo_user',
                           'ssh_connection', :'ssh_conn', 'host', :'host', 'script_sha256', :'script_sha', 'sql_sha256', :'sql_sha', 'backup_dir', :'backup_dir'));
SQL
}

start_ledger() { (cd "$INFRA_DIR" && "${COMPOSE[@]}" up -d --no-deps ledger); }
ledger_cmd() { echo "(cd $INFRA_DIR && docker compose -f docker-compose.dev.yml up -d --no-deps ledger)"; }

# ---- аварийный обработчик: EXIT покрывает die, set -e, Ctrl-C (INT), TERM и обрыв сессии (HUP до начала работы)
on_exit() {
  local rc=$?
  set +e                 # в аварийном обработчике выполняем каждый шаг, даже если предыдущий не удался (например, запись в закрытый терминал)
  trap - EXIT
  trap '' INT TERM HUP   # повторный Ctrl-C/TERM/обрыв сессии не должен оборвать возврат ledger
  [ "$rc" -ne 0 ] || return 0
  local epoch_row=""
  if [ "$STAGE" = "tx" ]; then   # транзакция могла пройти, а ответ потеряться: решает наличие метки с нашим request_id (он проверен на [A-Za-z0-9_-])
    if epoch_row=$(pg -c "SELECT epoch_id FROM ce_reset_epoch WHERE request_id = '$REQUEST_ID'" 2>/dev/null); then
      if [ -n "$epoch_row" ]; then NEW_EPOCH="$epoch_row"; STAGE="committed"; fi
    else
      STAGE="tx_unknown"      # сама проверка не удалась: «метки нет» утверждать нельзя
    fi
  fi
  echo >&2
  case "$STAGE" in
    preflight)
      echo "!! сброс не начат (код $rc): состояние не менялось." >&2
      if [ "$AUDIT_DENIALS" -eq 1 ] && [ -n "$PSQL_READY" ]; then audit_event ce_reset_absolute_denied "${DENY_MSG:-прервано до начала работы (код $rc)}" >/dev/null 2>&1 || true; fi;;
    stopping)
      echo "!! сбой до перемотки Kafka (код $rc): состояние не менялось — возвращаю ledger в работу." >&2
      start_ledger >&2 || echo "!! ledger не запустился — запустите вручную: $(ledger_cmd)" >&2   # сначала ledger, потом аудит: зависший Postgres не должен задерживать возврат
      audit_event ce_reset_absolute_failed "${DENY_MSG:-сбой до перемотки Kafka (код $rc); ledger возвращён}" >/dev/null 2>&1 || true;;
    seeking|tx)
      audit_event ce_reset_absolute_failed "${DENY_MSG:-сбой после перемотки Kafka (код $rc), метка не создана}" >/dev/null 2>&1 || true
      cat >&2 <<EOF
!! СБОЙ ПОСЛЕ ПЕРЕМОТКИ KAFKA (код $rc): ledger НАМЕРЕННО ОСТАНОВЛЕН.
   Сообщения, пришедшие до перемотки, пропущены, а Postgres НЕ обнулён (метка не создана) — запускать ledger на таком состоянии нельзя.
   Что сделать:
     1) повторите этот же запуск скрипта с теми же параметрами (--request-id $REQUEST_ID): он остановит ledger, перемотает группы и повторит транзакцию
        (если метка эпохи создана менее минуты назад — подождите минуту: сброс чаще раза в минуту отвергается);
     2) только если сброс решено отменить: запустите ledger вручную $(ledger_cmd) — позиции будут неполными (пропущенные сообщения потеряны);
        смещения групп до перемотки сохранены в $BK/group_*_before.txt.
   Журнал запуска: ${BK:+$BK/run.log}
EOF
      ;;
    tx_unknown)
      cat >&2 <<EOF
!! СОСТОЯНИЕ НЕИЗВЕСТНО (код $rc): ответ транзакции потерян и проверить наличие метки не удалось. Ledger НАМЕРЕННО ОСТАНОВЛЕН.
   Проверьте: docker exec $PG_CONTAINER psql -U cex -d cex -At -c "SELECT epoch_id FROM ce_reset_epoch WHERE request_id = '$REQUEST_ID'"
     - строка есть  → сброс ВЫПОЛНЕН: запустите ledger $(ledger_cmd), повторять сброс не нужно;
     - строки нет   → сброс не выполнен: повторите запуск с теми же параметрами (см. руководство ce-absolute-reset.md).
   Журнал запуска: ${BK:+$BK/run.log}
EOF
      ;;
    committed)
      audit_event ce_reset_absolute_failed "сброс выполнен (эпоха ${NEW_EPOCH:-?}), но ledger не запущен (код $rc)" >/dev/null 2>&1 || true
      cat >&2 <<EOF
!! СБРОС ВЫПОЛНЕН (эпоха ${NEW_EPOCH:-?}), но ledger НЕ ЗАПУЩЕН (код $rc). Запустите его: $(ledger_cmd)
   До запуска ledger позиции в Postgres уже нулевые, метка создана; повторять сброс не нужно.
EOF
      ;;
  esac
  exit "$rc"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# ---- проверки без обращения к системе
[ -n "$NOTE" ] || die "укажите название эксперимента: --note \"...\" (1-200 знаков)"
[ "$(printf 'я' | wc -m | tr -d ' ')" = "1" ] || die "нужна UTF-8 локаль (C.UTF-8): иначе длина названия считается неверно"
[ "${#NOTE}" -ge 1 ] && [ "${#NOTE}" -le 200 ] || die "название эксперимента должно быть 1-200 знаков (сейчас ${#NOTE})"
[ -n "$REQUEST_ID" ] || REQUEST_ID="abs-$(date -u +%Y%m%dT%H%M%S)-$RANDOM"
for v in "$NOTE" "$ACTOR" "$REQUEST_ID"; do
  [[ "$v" != *[[:cntrl:]]* ]] || die "управляющие символы (перевод строки, табуляция, NUL) в --note/--actor/--request-id не допускаются"
done
if printf '%s' "$NOTE" | grep -qP '[\p{Cc}\p{Cf}\p{Zl}\p{Zp}]'; then die "в --note недопустимы управляющие и форматные символы Unicode (в том числе смена направления текста)"; fi
[[ "$REQUEST_ID" =~ ^[A-Za-z0-9_-]{8,80}$ ]] || die "request-id: 8-80 знаков [A-Za-z0-9_-]"
[[ "$ACTOR" =~ ^[A-Za-z0-9._@-]{1,64}$ ]] || die "actor: 1-64 знака [A-Za-z0-9._@-]"
[ -f "$SQL_FILE" ] || die "нет файла транзакции $SQL_FILE (скопируйте его вместе со скриптом)"
[ "$(grep -c '^COMMIT;$' "$SQL_FILE")" = "1" ] || die "в $SQL_FILE строка COMMIT; должна встречаться ровно один раз"

SCRIPT_SHA=$(sha256sum "$0" | awk '{print $1}')
SQL_SHA=$(sha256sum "$SQL_FILE" | awk '{print $1}')
REMOTE_ADDR=$(printf '%s' "${SSH_CONNECTION:-}" | awk '{print $1}')
TS="$(date -u +%Y%m%dT%H%M%SZ)"
BK="$BACKUP_ROOT/$TS"
if [ "$YES" -eq 1 ] && [ "${CE_RESET_ALLOWED:-0}" = "1" ]; then AUDIT_DENIALS=1; fi

# ---- проверки системы (до любых изменений)
for c in "$PG_CONTAINER" "$RP_CONTAINER" "$VENUES_CONTAINER"; do
  [ "$(docker inspect -f '{{.State.Status}}' "$c" 2>/dev/null || true)" = "running" ] || die "контейнер $c не запущен (скрипт запускают на dev-хосте)"
done
# ledger может быть остановлен — это повторный запуск после сбоя; контейнер при этом должен существовать
case "$(docker inspect -f '{{.State.Status}}' "$LEDGER_CONTAINER" 2>/dev/null || true)" in
  running|exited|created) ;;
  *) die "контейнер $LEDGER_CONTAINER не найден (скрипт запускают на dev-хосте)";;
esac
SEEDS=$(docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$LEDGER_CONTAINER" | grep '^CE_CAPITAL_SEED_' | sort | tr '\n' ' ') || true

PSQL_VARS=(-v "actor=$ACTOR" -v "note=$NOTE" -v "rid=$REQUEST_ID" -v "backup_dir=$BK" -v "seeds=$SEEDS" -v "os_user=$(id -un)"
           -v "sudo_user=${SUDO_USER:-}" -v "ssh_conn=${SSH_CONNECTION:-}" -v "host=$(hostname)" -v "script_sha=$SCRIPT_SHA" -v "sql_sha=$SQL_SHA"
           -v "remote_addr=$REMOTE_ADDR")
PSQL_READY=1

# ---- один запуск за раз: замок берётся ДО проверок состояния, иначе второй запуск проверил бы «метки нет» раньше, чем первый её создаст
if [ "$YES" -eq 1 ] && [ "${CE_RESET_ALLOWED:-0}" = "1" ]; then
  mkdir -p "$BACKUP_ROOT"
  exec 9>"$BACKUP_ROOT/.absolute_reset.lock"
  flock -n 9 || die "уже выполняется другой абсолютный сброс (замок $BACKUP_ROOT/.absolute_reset.lock)"
fi

# признак песочницы: без симуляции заявок venues ходил бы на живые биржи, а у них нет контракта отмены (ADR-090 п. 4)
[ "$(docker exec "$VENUES_CONTAINER" printenv VENUES_SIMULATE_ORDERS 2>/dev/null || true)" = "1" ] \
  || die "это не песочница: в $VENUES_CONTAINER нет VENUES_SIMULATE_ORDERS=1 — сброс разрешён только при симуляции заявок"
pg -c "SELECT 1 FROM pg_proc WHERE proname = 'ce_epoch_open'" | grep -q 1 \
  || die "в Postgres нет схемы меток (функции ce_epoch_open): запустите BFF версии Э1 — он создаёт схему при первом обращении"
for t in ce_agent_position ce_agent_pnl_book f05a_clearing_config operator_audit_log ce_reset_epoch; do
  [ "$(pg -c "SELECT to_regclass('$t') IS NOT NULL")" = "t" ] || die "в Postgres нет таблицы $t"
done
USED=$(pg -c "SELECT count(*) FROM ce_reset_epoch WHERE request_id = '$REQUEST_ID'")
[ "$USED" = "0" ] || die "request-id $REQUEST_ID уже использован для созданной метки: повтор обнулил бы состояние без новой эпохи; укажите другой"
TOO_RECENT=$(pg -c "SELECT COALESCE(clock_timestamp() - max(reset_at) < interval '60 seconds', false) FROM ce_reset_epoch")
[ "$TOO_RECENT" = "f" ] || die "последняя метка создана менее минуты назад (повторный сброс подряд не допускается)"
CUR_EPOCH=$(pg -c "SELECT ce_current_epoch()")
# группы должны существовать: Dead означает, что перематывать нечего (имя группы устарело) — безопаснее отказаться до остановки ledger
group_state() {   # $1 = группа → печатает STATE; пустой вывод или ошибка rpk — ошибка (не «пусто»)
  local out st
  out=$(docker exec "$RP_CONTAINER" rpk group describe "$1" 2>&1) || return 1
  st=$(printf '%s\n' "$out" | awk '/^STATE/{print $2; exit}')
  [ -n "$st" ] || return 1
  printf '%s' "$st"
}
for g in "${GROUPS_TO_SEEK[@]}"; do
  st=$(group_state "$g") || die "не удалось прочитать состояние группы $g (rpk)"
  [ "$st" != "Dead" ] || die "группа $g в состоянии Dead (нет смещений): перематывать нечего — проверьте список GROUPS_TO_SEEK"
done
D="$BACKUP_ROOT"; [ -d "$D" ] || D="$(dirname "$D")"
FREE_KB=$(df -Pk "$D" | awk 'NR==2{print $4}')
[ "${FREE_KB:-0}" -ge "$MIN_FREE_KB" ] || die "мало места под срез: свободно ${FREE_KB:-0} КБ в $D, нужно не меньше $MIN_FREE_KB"

# ---- пробная транзакция: тот же SQL, COMMIT заменён откатом — ловит ошибку схемы/параметров ДО остановки ledger
DRY=$(sed 's/^COMMIT;$/ROLLBACK;/' "$SQL_FILE" | pg "${PSQL_VARS[@]}") || die "пробная транзакция не прошла (откатана, ничего не изменено): см. сообщение выше"

echo "== абсолютный сброс контура CE =="
echo "   название : $NOTE"
echo "   автор    : $ACTOR (os: $(id -un)${SUDO_USER:+, sudo: $SUDO_USER})   request_id: $REQUEST_ID"
echo "   текущая эпоха: $CUR_EPOCH   сиды ledger: ${SEEDS:-не найдены}"
echo "   группы Kafka к перемотке в конец: ${GROUPS_TO_SEEK[*]}"
echo "   проверки пройдены: песочница, схема, группы, место ($((FREE_KB / 1024)) МБ), пробная транзакция откатана"
echo "   план: аудит попытки → stop ledger → ждать пустые группы → срез → seek → [одна транзакция PG: метка + обнуление + аудит] → start ledger"

if [ "$YES" -ne 1 ] || [ "${CE_RESET_ALLOWED:-0}" != "1" ]; then
  echo
  echo "РЕЖИМ ПЛАНА: ничего не изменено. Для выполнения: CE_RESET_ALLOWED=1 $0 --note \"...\" --yes   (только песочница)"
  exit 0
fi

# ---- с этого места работа идёт до конца: попытка записана в аудит, вывод дублируется в журнал, обрыв сессии (HUP) не останавливает
audit_event ce_reset_absolute_attempt "начало абсолютного сброса; эпоха до сброса $CUR_EPOCH" >/dev/null || die "не удалось записать попытку в operator_audit_log — без аудита не начинаю"
mkdir -p "$BK"
# tee запускается в отдельной сессии (setsid): Ctrl-C приходит всей группе процессов терминала и убил бы обычный tee — аварийный обработчик остался бы
# с закрытым каналом и не смог бы напечатать инструкции; при обрыве SSH tee продолжает писать в журнал (--output-error=warn)
exec > >(exec setsid tee --output-error=warn -a "$BK/run.log") 2>&1
trap '' HUP PIPE
echo "   журнал запуска: $BK/run.log"

wait_group_empty() {   # $1 = группа: у остановленного ledger группа Empty; Dead или неизвестное состояние — отказ
  local g="$1" st="" i
  for i in $(seq 1 60); do
    st=$(group_state "$g") || die "не удалось прочитать состояние группы $g (rpk)"
    case "$st" in
      Empty) return 0;;
      Dead) die "группа $g стала Dead: перематывать нечего";;
    esac
    sleep 2
  done
  die "группа $g не опустела за 120 с (состояние: $st)"
}
dump_table() {   # $1 = таблица (константа из списка ниже): выгрузка в CSV и сверка числа строк
  local t="$1" want got
  want=$(pg -c "SELECT count(*) FROM $t")
  pg -c "COPY (SELECT * FROM $t) TO STDOUT WITH CSV HEADER" > "$BK/$t.csv"
  got=$(( $(wc -l < "$BK/$t.csv") - 1 ))
  [ "$got" -eq "$want" ] || die "срез $t неполон: в файле $got строк, в таблице $want"
}

# ---- остановить ledger и дождаться пустых групп (флаг — ДО команды: прерывание посреди stop тоже возвращает ledger)
STAGE="stopping"
(cd "$INFRA_DIR" && "${COMPOSE[@]}" stop ledger)
for g in "${GROUPS_TO_SEEK[@]}"; do wait_group_empty "$g"; done

# ---- состояние за время ожидания могло измениться (BFF создал метку кнопкой): перепроверка до перемотки, ещё можно вернуть ledger без потерь
[ "$(pg -c "SELECT ce_current_epoch()")" = "$CUR_EPOCH" ] || die "за время остановки ledger создана новая эпоха (была $CUR_EPOCH): повторите запуск"
[ "$(pg -c "SELECT count(*) FROM ce_reset_epoch WHERE request_id = '$REQUEST_ID'")" = "0" ] || die "request-id $REQUEST_ID за время остановки ledger уже использован"
[ "$(pg -c "SELECT COALESCE(clock_timestamp() - max(reset_at) < interval '60 seconds', false) FROM ce_reset_epoch")" = "f" ] || die "метка создана менее минуты назад: подождите минуту и повторите запуск"

# ---- срез состояния ПОСЛЕ остановки ledger (пишет в эти таблицы только он): ничего не потеряно между срезом и обнулением
for g in "${GROUPS_TO_SEEK[@]}"; do docker exec "$RP_CONTAINER" rpk group describe "$g" > "$BK/group_${g}_before.txt"; done
for t in ce_agent_position ce_agent_pnl_book ce_reset_epoch; do dump_table "$t"; done
if [ "$(pg -c "SELECT to_regclass('ce_system_pnl_baseline') IS NOT NULL")" = "t" ]; then dump_table ce_system_pnl_baseline; fi
pg -c "COPY (SELECT trade_stats_reset_at, config_version FROM f05a_clearing_config WHERE id = 1) TO STDOUT WITH CSV HEADER" > "$BK/f05a_clearing_config_reset.csv"
sync
echo "   срез сохранён: $BK"

# ---- перемотка: с первой команды ledger автоматически не возвращается (сообщения уже теряются)
STAGE="seeking"
for g in "${GROUPS_TO_SEEK[@]}"; do docker exec "$RP_CONTAINER" rpk group seek "$g" --to end; done

# ---- одна транзакция: метка + обнуление состояния + запасная метка + аудит
STAGE="tx"
OUT=$(pg "${PSQL_VARS[@]}" < "$SQL_FILE")
NEW_EPOCH=$(printf '%s\n' "$OUT" | sed -n 's/^эпоха \([0-9][0-9]*\) создана.*/\1/p' | tail -1)
[ -n "$NEW_EPOCH" ] || NEW_EPOCH=$(pg -c "SELECT epoch_id FROM ce_reset_epoch WHERE request_id = '$REQUEST_ID'")
STAGE="committed"
echo "   $(printf '%s\n' "$OUT" | tail -1)"

# ---- запустить ledger (в памяти — исходное состояние: остатки площадок, план, очереди, пороги)
start_ledger
sleep 5
[ "$(docker inspect -f '{{.State.Status}}' "$LEDGER_CONTAINER")" = "running" ] || die "ledger не в состоянии running после запуска"
STAGE="done"
audit_event ce_reset_absolute_done "сброс завершён; эпоха $NEW_EPOCH; ledger запущен" >/dev/null 2>&1 || true
echo "== готово: эпоха $NEW_EPOCH, ledger запущен. Проверьте экран «Клиринг»: позиции с нуля, в «Баланс» план по активу = 0, эпоха — GET /api/vector-clearing/epochs"
echo "   Первый час эпохи (прогрев, CE_EPOCH_WARMUP_MIN) модель порогов ADR-088 копит оценки потока: экраны подписывают его, скрипты разбора по умолчанию исключают."
echo "   Режим модели при необходимости переключите в shadow."
