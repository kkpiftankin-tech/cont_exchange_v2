#!/usr/bin/env bash
# ============================================================================
# CE F-18 — проверка учётных инвариантов (этап 1 плана F-18-remediation).
#
# Гоняет на ЖИВОМ dev-стеке (infra/docker-compose.dev.yml), читает PostgreSQL
# ce_agent_position напрямую. Это диагностика учёта, а не изменение поведения.
#
# Инвариант 4.1 (CALC-CE-SYSTEM-PNL §4.1) — узловой баланс W·c=0:
#   для каждого узла (asset,venue) сумма позиций ВСЕХ агентов = 0.
#   До этапа 2 (парная проводка ног арбитражёра) ТЕСТ КРАСНЫЙ — это ожидаемо и
#   документирует причину 3. После этапа 2 должен позеленеть.
#
# Инварианты 4.2 (c+in_flight=I−F) и 4.3 (Σ P&L+дом=P&L системы) — требуют
# инструментации этапа 1 (per-agent I/F и MTM), пока помечены TODO.
#
# Использование (на dev-хосте):
#   bash Testing/ce_invariants_check.sh            # порог EPS=1.0 (k-USDT)
#   EPS=0.01 bash Testing/ce_invariants_check.sh   # строгий порог
#   PSQL="psql -U cex -d cex" bash Testing/...      # кастомный доступ к БД
# exit 0 — инвариант держится; exit 1 — нарушен (CI-пригодно).
# ============================================================================
set -uo pipefail

# Порог нарушения узла в k-USDT (позиции хранятся в тысячах USDT).
EPS="${EPS:-1.0}"
# Доступ к БД: по умолчанию через контейнер dev-стека.
PSQL="${PSQL:-docker exec -i infra-postgres-1 psql -U cex -d cex}"

echo "== CE инвариант W·c=0 (узловой баланс), порог EPS=${EPS} k-USDT =="

# Топ узлов-нарушителей: Σ позиций агентов по (asset,venue) против нуля.
$PSQL -P pager=off -c "
  SELECT asset, venue,
         round(sum(position), 4)       AS node_sum_wc,
         count(*)                       AS agents,
         round(sum(abs(position)), 3)   AS gross
  FROM ce_agent_position
  GROUP BY asset, venue
  HAVING abs(sum(position)) > ${EPS}
  ORDER BY abs(sum(position)) DESC
  LIMIT 20;"

# Итоговая метрика: число нарушающих узлов и максимум |нетто|.
read -r VIOL MAXABS <<< "$($PSQL -tA -F' ' -c "
  SELECT count(*), COALESCE(round(max(abs(s)),4), 0)
  FROM (SELECT sum(position) s FROM ce_agent_position GROUP BY asset, venue) q
  WHERE abs(s) > ${EPS};")"

echo "-- nodes_violating=${VIOL} max_abs=${MAXABS} (порог ${EPS}) --"
echo "TODO (этап 1): инвариант 4.2 c+in_flight=I−F и 4.3 Σ P&L+дом=P&L системы —"
echo "              требуют per-agent I/F и MTM-учёта (пока не инструментированы)."

if [[ "${VIOL:-0}" -gt 0 ]]; then
  echo "РЕЗУЛЬТАТ: КРАСНЫЙ — W·c=0 нарушен в ${VIOL} узлах (ожидаемо до этапа 2)."
  exit 1
fi
echo "РЕЗУЛЬТАТ: ЗЕЛЁНЫЙ — узловой баланс держится."
exit 0
