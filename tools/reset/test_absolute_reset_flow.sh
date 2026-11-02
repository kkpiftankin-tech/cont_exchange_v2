#!/bin/bash
# Проверка потока и аварийных этапов absolute_reset.sh БЕЗ настоящих контейнеров: вместо docker подставляется заглушка (docker/sleep в PATH первыми,
# DOCKER_HOST указывает в никуда), которая ведёт журнал вызовов и по переменным STUB_* имитирует сбои. Проверяется главное свойство безопасности:
#   до перемотки Kafka  — ledger возвращается;  после перемотки — НЕ запускается и печатаются инструкции;  после COMMIT — «сброс выполнен».
# Нужен Linux с GNU-утилитами (flock, setsid, tee --output-error): запуск на dev-хосте:
#   rsync -a tools/reset/ nik@ubuntu-dev:/tmp/reset-flow-test/ && ssh nik@ubuntu-dev 'bash /tmp/reset-flow-test/test_absolute_reset_flow.sh'
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d /tmp/reset-flow.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
STUBBIN="$WORK/bin"; mkdir -p "$STUBBIN" "$WORK/tool" "$WORK/infra"
cp "$HERE/absolute_reset.sh" "$HERE/absolute_reset_tx.sql" "$WORK/tool/"
chmod +x "$WORK/tool/absolute_reset.sh"

cat > "$STUBBIN/sleep" <<'EOF'
#!/bin/bash
exit 0
EOF
cat > "$STUBBIN/docker" <<'EOF'
#!/bin/bash
# заглушка docker: состояние в $STUB_STATE; STUB_* управляют отказами
S="$STUB_STATE"
echo "docker $*" >> "$S/calls.log"
rpk() {
  case "$1 $2" in
    "group describe")
      if [ -n "${STUB_GROUP_STATE:-}" ]; then st="$STUB_GROUP_STATE"
      elif [ "$(cat "$S/ledger_state")" = running ] || [ -n "${STUB_GROUP_STUCK:-}" ]; then st=Stable; else st=Empty; fi
      printf 'GROUP %s\nCOORDINATOR 0\nSTATE %s\n' "$3" "$st";;
    "group seek")
      echo "seek $3" >> "$S/events.log"
      n=$(grep -c '^seek ' "$S/events.log")
      [ "${STUB_FAIL_SEEK_N:-0}" = "$n" ] && { echo "seek failed" >&2; exit 1; }
      [ -n "${STUB_INT_ON_SEEK:-}" ] && kill -INT 0;;
  esac
}
psql() {
  local sql="" a; local -a args=("$@")
  for i in "${!args[@]}"; do [ "${args[$i]}" = "-c" ] && sql="${args[$((i+1))]}"; done
  if [ -n "$sql" ]; then
    case "$sql" in
      *pg_proc*) echo 1;;
      *to_regclass*) echo t;;
      *"count(*) FROM ce_reset_epoch WHERE request_id"*) if [ -n "${STUB_RID_USED:-}" ] || [ -e "$S/committed" ]; then echo 1; else echo 0; fi;;
      *"epoch_id FROM ce_reset_epoch WHERE request_id"*) [ -n "${STUB_CHECK_FAIL:-}" ] && { echo "check failed" >&2; exit 1; }; [ -e "$S/committed" ] && echo 4;;
      *clock_timestamp*) echo "${STUB_RECENT:-f}";;
      *ce_current_epoch*) echo x >> "$S/epoch_calls"; if [ -n "${STUB_EPOCH_CHANGE:-}" ] && [ "$(wc -l < "$S/epoch_calls")" -ge 2 ]; then echo 4; else echo 3; fi;;
      "COPY (SELECT * FROM"*) printf 'h\nr1\nr2\n';;
      "COPY"*) printf 'a,b\n1,2\n';;
      *"SELECT count(*) FROM"*) echo 2;;
    esac
    return 0
  fi
  local input; input="$(cat)"
  case " ${args[*]} " in
    *ev_action=*) for a in "${args[@]}"; do case "$a" in ev_action=*) echo "AUDIT ${a#ev_action=}" >> "$S/events.log";; esac; done; return 0;;
  esac
  case "$input" in
    *"ROLLBACK;"*) echo "эпоха 4 создана (проба)"; return 0;;
  esac
  [ -n "${STUB_FAIL_TX:-}" ] && { echo "ERROR: tx failed" >&2; exit 3; }
  echo commit >> "$S/events.log"; : > "$S/committed"
  [ -n "${STUB_FAIL_TX_AFTER_COMMIT:-}" ] && { echo "connection lost" >&2; exit 3; }
  echo "эпоха 4 создана 2026-11-02T00:00:00.000000Z"
}
case "$1" in
  inspect)
    case "$3" in
      *State.Status*) case "$4" in *ledger*) cat "$S/ledger_state";; *) echo running;; esac;;
      *Config.Env*) printf 'CE_CAPITAL_SEED_BTC=10\nCE_CAPITAL_SEED_USDT=1000000\nOTHER=1\n';;
    esac;;
  compose)
    case "$*" in
      *" stop ledger"*) echo stop >> "$S/events.log"; echo exited > "$S/ledger_state"; [ -n "${STUB_INT_ON_STOP:-}" ] && kill -INT 0;;
      *"up -d --no-deps ledger"*) [ -n "${STUB_FAIL_UP:-}" ] && { echo "up failed" >&2; exit 1; }; echo up >> "$S/events.log"; echo running > "$S/ledger_state";;
    esac;;
  exec)
    shift
    while [ "$1" = -i ] || [ "$1" = -e ]; do if [ "$1" = -e ]; then shift 2; else shift; fi; done
    name="$1"; shift
    case "$name" in
      *venues*) echo "${STUB_SIM:-1}";;
      *redpanda*) shift; rpk "$@";;
      *postgres*) shift; psql "$@";;
    esac;;
esac
exit 0
EOF
chmod +x "$STUBBIN/docker" "$STUBBIN/sleep"

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "[OK]   $1"; }
bad()  { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# run NAME [env KEY=VAL ...] -- args...   → RC, OUT, EV (события), CALLS
run() {
  local name="$1"; shift
  local -a envs=()
  while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
  STATE="$WORK/state-$name"; rm -rf "$STATE"; mkdir -p "$STATE"; echo running > "$STATE/ledger_state"; : > "$STATE/events.log"; : > "$STATE/calls.log"
  OUT=$(setsid -w env PATH="$STUBBIN:/usr/bin:/bin" DOCKER_HOST=unix:///nonexistent STUB_STATE="$STATE" INFRA_DIR="$WORK/infra" BACKUP_ROOT="$WORK/backups-$name" \
        "${envs[@]}" "$WORK/tool/absolute_reset.sh" "$@" 2>&1)
  RC=$?
  EV="$(cat "$STATE/events.log")"; CALLS="$(cat "$STATE/calls.log")"; LEDGER="$(cat "$STATE/ledger_state")"
}
has() { printf '%s\n' "$1" | grep -q -- "$2"; }
idx() { printf '%s\n' "$EV" | grep -n -- "$1" | head -1 | cut -d: -f1; }
GOOD=(--note "опыт проверки" --actor tester --request-id test-rid-0001)
ALLOW=(CE_RESET_ALLOWED=1)

[ "$(PATH="$STUBBIN:/usr/bin:/bin" type -P docker)" = "$STUBBIN/docker" ] || { echo "ОШИБКА: docker не подменён — тест остановлен"; exit 1; }

# 1) режим плана: проверки и откат пробной транзакции, ни остановки, ни перемотки
run plan "${ALLOW[@]}" -- "${GOOD[@]}"
check "план: код 0 и сообщение о режиме плана" '[ $RC -eq 0 ] && has "$OUT" "РЕЖИМ ПЛАНА"'
check "план: ledger не останавливался, перемотки и COMMIT не было" '! has "$EV" "^stop" && ! has "$EV" "^seek" && ! has "$EV" "^commit"'
check "план: пробная транзакция выполнена" 'has "$OUT" "пробная транзакция откатана"'

# 2) без CE_RESET_ALLOWED --yes тоже только план
run noallow -- "${GOOD[@]}" --yes
check "без CE_RESET_ALLOWED=1 --yes не выполняет сброс" '[ $RC -eq 0 ] && has "$OUT" "РЕЖИМ ПЛАНА" && ! has "$EV" "^stop"'

# 3) счастливый путь: порядок stop → seek → commit → up, аудит попытки и завершения, срез и журнал
run happy "${ALLOW[@]}" -- "${GOOD[@]}" --yes
check "успех: код 0, эпоха названа" '[ $RC -eq 0 ] && has "$OUT" "эпоха 4"'
check "успех: порядок stop < seek < commit < up" '[ "$(idx "^stop")" -lt "$(idx "^seek")" ] && [ "$(idx "^seek")" -lt "$(idx "^commit")" ] && [ "$(idx "^commit")" -lt "$(idx "^up")" ]'
check "успех: перемотаны все три группы" '[ "$(printf "%s\n" "$EV" | grep -c "^seek")" = 3 ]'
check "успех: аудит попытки и завершения" 'has "$EV" "AUDIT ce_reset_absolute_attempt" && has "$EV" "AUDIT ce_reset_absolute_done"'
check "успех: ledger запущен" '[ "$LEDGER" = running ]'
check "успех: срез и журнал сохранены" 'ls "$WORK"/backups-happy/*/ce_agent_position.csv >/dev/null 2>&1 && ls "$WORK"/backups-happy/*/run.log >/dev/null 2>&1 && ls "$WORK"/backups-happy/*/group_ledger-exec_before.txt >/dev/null 2>&1'
check "успех: каталог среза закрыт для остальных (umask 077)" '[ "$(stat -c %a "$(ls -d "$WORK"/backups-happy/*/ | head -1)")" = 700 ]'

# 4) сбой ДО перемотки: группа не опустела → ledger возвращается, перемотки нет
run stuck "${ALLOW[@]}" STUB_GROUP_STUCK=1 -- "${GOOD[@]}" --yes
check "группа не опустела: отказ, ledger возвращён в работу" '[ $RC -ne 0 ] && [ "$LEDGER" = running ] && has "$EV" "^up" && ! has "$EV" "^seek"'
check "группа не опустела: сбой записан в аудит" 'has "$EV" "AUDIT ce_reset_absolute_failed"'

# 5) прерывание (Ctrl-C) во время остановки ledger — тоже до перемотки: ledger возвращается
run intstop "${ALLOW[@]}" STUB_INT_ON_STOP=1 -- "${GOOD[@]}" --yes
check "INT при остановке: код 130, ledger возвращён, перемотки нет" '[ $RC -eq 130 ] && [ "$LEDGER" = running ] && ! has "$EV" "^seek"'

# 6) сбой ПОСЛЕ перемотки: транзакция не прошла → ledger НЕ запускается, печатаются инструкции
run txfail "${ALLOW[@]}" STUB_FAIL_TX=1 -- "${GOOD[@]}" --yes
check "сбой транзакции после seek: ledger остаётся остановленным" '[ $RC -ne 0 ] && [ "$LEDGER" = exited ] && ! has "$EV" "^up"'
check "сбой транзакции после seek: инструкции и повтор с тем же request-id" 'has "$OUT" "НАМЕРЕННО ОСТАНОВЛЕН" && has "$OUT" "test-rid-0001"'

# 7) сбой перемотки на второй группе → ledger НЕ запускается
run seekfail "${ALLOW[@]}" STUB_FAIL_SEEK_N=2 -- "${GOOD[@]}" --yes
check "сбой перемотки второй группы: ledger остаётся остановленным" '[ $RC -ne 0 ] && [ "$LEDGER" = exited ] && ! has "$EV" "^up" && has "$OUT" "НАМЕРЕННО ОСТАНОВЛЕН"'

# 8) Ctrl-C после начала перемотки → ledger НЕ запускается
run intseek "${ALLOW[@]}" STUB_INT_ON_SEEK=1 -- "${GOOD[@]}" --yes
check "INT после начала перемотки: ledger остаётся остановленным" '[ $RC -eq 130 ] && [ "$LEDGER" = exited ] && ! has "$EV" "^up" && has "$OUT" "НАМЕРЕННО ОСТАНОВЛЕН"'

# 9) транзакция прошла, ответ потерян: по наличию метки понимаем, что сброс выполнен
run lost "${ALLOW[@]}" STUB_FAIL_TX_AFTER_COMMIT=1 -- "${GOOD[@]}" --yes
check "ответ транзакции потерян после COMMIT: «сброс выполнен», а не «повторите»" '[ $RC -ne 0 ] && has "$OUT" "СБРОС ВЫПОЛНЕН (эпоха 4)" && ! has "$OUT" "НАМЕРЕННО ОСТАНОВЛЕН"'

# 10) COMMIT прошёл, ledger не поднялся
run upfail "${ALLOW[@]}" STUB_FAIL_UP=1 -- "${GOOD[@]}" --yes
check "ledger не поднялся после COMMIT: сообщение «сброс выполнен, ledger не запущен»" '[ $RC -ne 0 ] && has "$OUT" "СБРОС ВЫПОЛНЕН (эпоха 4), но ledger НЕ ЗАПУЩЕН"'

# 11) отказы предполётных проверок: ничего не остановлено, отказ записан в аудит
run nosandbox "${ALLOW[@]}" STUB_SIM=0 -- "${GOOD[@]}" --yes
check "не песочница: отказ без остановки ledger, отказ в аудите" '[ $RC -ne 0 ] && has "$OUT" "это не песочница" && ! has "$EV" "^stop" && has "$EV" "AUDIT ce_reset_absolute_denied"'
run ridused "${ALLOW[@]}" STUB_RID_USED=1 -- "${GOOD[@]}" --yes
check "повтор request-id: отказ до остановки" '[ $RC -ne 0 ] && has "$OUT" "уже использован" && ! has "$EV" "^stop"'
run recent "${ALLOW[@]}" STUB_RECENT=t -- "${GOOD[@]}" --yes
check "метка моложе минуты: отказ до остановки" '[ $RC -ne 0 ] && has "$OUT" "менее минуты" && ! has "$EV" "^stop"'
run dead "${ALLOW[@]}" STUB_GROUP_STATE=Dead -- "${GOOD[@]}" --yes
check "группа Dead в предполёте: отказ до остановки" '[ $RC -ne 0 ] && has "$OUT" "Dead" && ! has "$EV" "^stop"'
run badrid "${ALLOW[@]}" -- --note "x" --actor tester --request-id $'valid-rid-1\ninjected' --yes
check "request-id с переводом строки отвергнут" '[ $RC -ne 0 ] && ! has "$EV" "^stop"'
run ctrl "${ALLOW[@]}" -- --note $'a\tb' --actor tester --yes
check "табуляция в названии отвергнута" '[ $RC -ne 0 ] && has "$OUT" "управляющие символы" && ! has "$EV" "^stop"'
run badactor "${ALLOW[@]}" -- --note "x" --actor "evil actor;" --yes
check "актор с недопустимыми знаками отвергнут" '[ $RC -ne 0 ] && has "$OUT" "actor:" && ! has "$EV" "^stop"'
run nonote "${ALLOW[@]}" -- --yes
check "без названия эксперимента — отказ" '[ $RC -ne 0 ] && has "$OUT" "название эксперимента"'
run longnote "${ALLOW[@]}" -- --note "$(printf 'я%.0s' $(seq 1 201))" --yes
check "название длиннее 200 знаков отвергнуто" '[ $RC -ne 0 ] && has "$OUT" "1-200 знаков"'
mv "$WORK/tool/absolute_reset_tx.sql" "$WORK/tool/absolute_reset_tx.sql.off"
run nosql "${ALLOW[@]}" -- "${GOOD[@]}" --yes
mv "$WORK/tool/absolute_reset_tx.sql.off" "$WORK/tool/absolute_reset_tx.sql"
check "нет файла транзакции: отказ ДО остановки ledger" '[ $RC -ne 0 ] && has "$OUT" "нет файла транзакции" && ! has "$EV" "^stop"'

# 11a) эпоха изменилась за время остановки ledger (BFF создал метку кнопкой): перепроверка до перемотки возвращает ledger
run epochmoved "${ALLOW[@]}" STUB_EPOCH_CHANGE=1 -- "${GOOD[@]}" --yes
check "эпоха изменилась за время остановки: отказ до перемотки, ledger возвращён" '[ $RC -ne 0 ] && has "$OUT" "создана новая эпоха" && ! has "$EV" "^seek" && [ "$LEDGER" = running ]'

# 11b) ответ транзакции потерян и проверка метки тоже не удалась: «состояние неизвестно», ledger остаётся остановленным
run unknown "${ALLOW[@]}" STUB_FAIL_TX_AFTER_COMMIT=1 STUB_CHECK_FAIL=1 -- "${GOOD[@]}" --yes
check "проверка метки не удалась: «состояние неизвестно», а не «метки нет»" '[ $RC -ne 0 ] && has "$OUT" "СОСТОЯНИЕ НЕИЗВЕСТНО" && ! has "$OUT" "СБРОС ВЫПОЛНЕН" && ! has "$EV" "^up" && [ "$LEDGER" = exited ]'

# 11c) в аварийной ветке до перемотки ledger возвращается РАНЬШЕ записи в аудит (зависший Postgres не должен задерживать возврат)
run intstop2 "${ALLOW[@]}" STUB_INT_ON_STOP=1 -- "${GOOD[@]}" --yes
check "INT при остановке (повтор): порядок up < аудит сбоя" '[ "$(idx "^up")" -lt "$(idx "AUDIT ce_reset_absolute_failed")" ]'

# 11d) форматные символы Unicode в названии (смена направления текста) отвергнуты
run bidi "${ALLOW[@]}" -- --note $'опыт\xe2\x80\xaeгэ' --actor tester --yes
check "форматный символ Unicode в названии отвергнут" '[ $RC -ne 0 ] && has "$OUT" "форматные символы" && ! has "$EV" "^stop"'

# 12) параметр без значения
run noval -- --note
check "--note без значения: код 2" '[ $RC -eq 2 ]'

echo
echo "итого: прошло $PASS, провалено $FAIL"
[ "$FAIL" -eq 0 ]
