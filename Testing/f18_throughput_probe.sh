#!/usr/bin/env bash
# F-18 T-F18-603 — повторяемая проверка throughput-инварианта консьюмера venues_exec.
#
# Инвариант (после T-F18-601 «пул PG-соединений»): под нагрузкой band-эмиссии
#   1) TOTAL-LAG группы venues_exec НЕ растёт монотонно (консьюмер тянет поток), и
#   2) в логах venues 0 падающих PG-операций (child_orders/hedgeflows write failed).
#
# Раньше (соединение-на-запись) потолок консьюмера был ~2.8/с, при эмиссии ~6.6/с
# бэклог рос до десятков тысяч, in_flight «утекал». Скрипт стрессирует систему,
# временно снижая CE_BAND_HEDGE_COOLDOWN_MS (эмиссия взлетает), и проверяет lag.
#
# Запуск НА dev-хосте (есть docker + rpk), против уже поднятого стека:
#   bash Testing/f18_throughput_probe.sh
# Переменные:
#   F18_STRESS_COOLDOWN_MS  cooldown на время стресса (по умолч. 500; 0 = выкл)
#   F18_WARMUP_SEC          прогрев после пересоздания ledger (по умолч. 20)
#   F18_MEASURE_SEC         длительность замера (по умолч. 90)
#   F18_SAMPLE_SEC          период выборки lag (по умолч. 15)
#   F18_LAG_LIMIT           допустимый рост lag между первой и последней выборкой
#                           (по умолч. 500 — небольшой дребезг допустим)
#   F18_RESTORE_COOLDOWN_MS cooldown после теста (по умолч. читаем из compose)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE="${ROOT_DIR}/infra/docker-compose.dev.yml"

STRESS_COOLDOWN_MS="${F18_STRESS_COOLDOWN_MS:-500}"
WARMUP_SEC="${F18_WARMUP_SEC:-20}"
MEASURE_SEC="${F18_MEASURE_SEC:-90}"
SAMPLE_SEC="${F18_SAMPLE_SEC:-15}"
LAG_LIMIT="${F18_LAG_LIMIT:-500}"

RP="infra-redpanda-1"     # контейнер Redpanda (rpk внутри)
VENUES="infra-venues-1"   # контейнер venues (логи PG-ошибок)
LEDGER="infra-ledger-1"   # контейнер ledger (эмиссия band)

# читаем текущее значение cooldown из compose, чтобы вернуть его в конце
ORIG_COOLDOWN="$(grep -oE 'CE_BAND_HEDGE_COOLDOWN_MS: "[0-9]+"' "${COMPOSE}" | grep -oE '[0-9]+' | head -1 || echo 2000)"
RESTORE_COOLDOWN_MS="${F18_RESTORE_COOLDOWN_MS:-${ORIG_COOLDOWN}}"

lag_now() {  # суммарный TOTAL-LAG группы venues_exec
  docker exec "${RP}" rpk group describe venues_exec 2>/dev/null \
    | grep 'TOTAL-LAG' | awk '{print $2}'
}

set_cooldown() {  # $1 = значение мс; правит compose и пересоздаёт ledger
  sed -i -E "s/CE_BAND_HEDGE_COOLDOWN_MS: \"[0-9]+\"/CE_BAND_HEDGE_COOLDOWN_MS: \"$1\"/" "${COMPOSE}"
  docker compose -f "${COMPOSE}" up -d --no-build --force-recreate ledger >/dev/null 2>&1
}

restore() {  # вернуть исходный cooldown при любом выходе
  echo "[cleanup] возвращаю CE_BAND_HEDGE_COOLDOWN_MS=${RESTORE_COOLDOWN_MS}"
  set_cooldown "${RESTORE_COOLDOWN_MS}" || true
}
trap restore EXIT

echo "=== F-18 throughput probe: стресс cooldown=${STRESS_COOLDOWN_MS}мс ==="
set_cooldown "${STRESS_COOLDOWN_MS}"
echo "[warmup] ${WARMUP_SEC}с..."; sleep "${WARMUP_SEC}"

first_lag=""; last_lag=""; max_lag=0; elapsed=0
while [ "${elapsed}" -lt "${MEASURE_SEC}" ]; do
  sleep "${SAMPLE_SEC}"; elapsed=$((elapsed + SAMPLE_SEC))
  lag="$(lag_now)"; lag="${lag:-0}"
  emit="$(docker logs --since "${SAMPLE_SEC}s" "${LEDGER}" 2>&1 | grep -c 'band breach emitted' || true)"
  [ -z "${first_lag}" ] && first_lag="${lag}"
  last_lag="${lag}"
  [ "${lag}" -gt "${max_lag}" ] && max_lag="${lag}"
  printf "t=%3ss  emit/%ss=%-4s  venues_exec_LAG=%s\n" "${elapsed}" "${SAMPLE_SEC}" "${emit}" "${lag}"
done

# PG-ошибки записи за окно замера
pg_errs="$(docker logs --since "${MEASURE_SEC}s" "${VENUES}" 2>&1 \
  | grep -cE 'Failed to (insert|apply report to) (child_order|hedgeflow)' || true)"

lag_growth=$(( last_lag - first_lag ))
echo "--- итог ---"
echo "first_lag=${first_lag} last_lag=${last_lag} growth=${lag_growth} max_lag=${max_lag} pg_write_errors=${pg_errs}"

fail=0
if [ "${lag_growth}" -gt "${LAG_LIMIT}" ]; then
  echo "FAIL: lag вырос на ${lag_growth} (> ${LAG_LIMIT}) — консьюмер не тянет поток"; fail=1
fi
if [ "${pg_errs}" -gt 0 ]; then
  echo "FAIL: ${pg_errs} падающих PG-операций записи в venues"; fail=1
fi
if [ "${fail}" -eq 0 ]; then
  echo "PASS: lag ограничен (рост ${lag_growth} ≤ ${LAG_LIMIT}), PG-ошибок 0 — T-F18-601 держит throughput"
fi
exit "${fail}"
