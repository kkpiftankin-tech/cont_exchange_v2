#!/usr/bin/env bash
# Задать срок хранения (TTL) всем аналитическим таблицам ClickHouse одной командой.
#
#   tools/clickhouse-retention/set_retention.sh <дней>            # показать ALTER-ы, ничего не менять
#   tools/clickhouse-retention/set_retention.sh <дней> --apply    # выполнить
#
# Запускать на хосте, где доступен ClickHouse (по умолчанию http://localhost:8123/, иначе CH_URL=...).
# Подробности и список таблиц: docs/07-data/clickhouse-retention.md.
#
# Важно:
#  - УМЕНЬШЕНИЕ срока удаляет данные необратимо (ClickHouse сразу запускает мутацию материализации TTL).
#  - Три сервиса выставляют TTL своих таблиц при каждом старте по переменным окружения; задайте те же дни и там, иначе перезапуск
#    вернёт старое значение: CLICKHOUSE_LIQUIDITY_CURVES_RETENTION_DAYS (liquidity_curves), VENUES_CLICKHOUSE_RETENTION_DAYS
#    (backtest.venue_snapshots), BACKTEST_CLICKHOUSE_REPLAY_AGENTLOGS_RETENTION_DAYS (replay_agentlogs).
#  - liquidity_curves секционирована по дню: срок меняется тем же ALTER, удаление идёт целыми секциями.
#  - Не трогаются таблицы с намеренно коротким сроком: vector_flow_segments_history (2 дня), backtest.venue_book_live (2 часа).
set -euo pipefail

DAYS="${1:-}"
MODE="${2:-}"
CH_URL="${CH_URL:-http://localhost:8123/}"

if ! [[ "$DAYS" =~ ^[0-9]+$ ]] || [ "$DAYS" -lt 1 ] || [ "$DAYS" -gt 3650 ]; then
  echo "usage: $0 <дней 1..3650> [--apply]" >&2
  exit 2
fi

MS="toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL ${DAYS} DAY"   # колонка времени в миллисекундах
TS="toDateTime(timestamp) + INTERVAL ${DAYS} DAY"                      # колонка timestamp (DateTime64)

# таблица|выражение TTL
ITEMS=(
  "default.liquidity_curves|toDateTime(timestamp_ms / 1000) + INTERVAL ${DAYS} DAY"
  "default.vector_clearing_results|${MS}"
  "default.batchresults|${MS}"
  "default.batch_metrics|${MS}"
  "default.execution_venue|${MS}"
  "default.venue_snapshots|${MS}"
  "default.venue_liquidity_curves|${MS}"
  "backtest.venue_snapshots|${MS}"
  "default.backtest_batchresults|${MS}"
  "default.backtest_fills|${MS}"
  "default.execution_reports|${MS}"
  "default.fill_metrics|${MS}"
  "default.fills|${MS}"
  "default.grouped_execution_events|${MS}"
  "default.grouped_leg_fills|${MS}"
  "default.grouped_quality_metrics|${MS}"
  "default.grouped_ratio_deviation|${MS}"
  "default.replay_agentlogs|${MS}"
  "default.sim_divergence_log|${MS}"
  "default.sim_execution_reports|${MS}"
  "default.OrderBook|${TS}"
  "default.marketdata_snapshots|${TS}"
  "default.effective_spreads|${TS}"
  "default.hedge_pnl_agg|batch_time + INTERVAL ${DAYS} DAY"
  "default.venue_quality_reports|toDateTime(intDiv(to_ms, 1000)) + INTERVAL ${DAYS} DAY"
)

echo "Срок хранения: ${DAYS} дн.; ClickHouse: ${CH_URL}"
[ "$MODE" = "--apply" ] || echo "(режим просмотра: ничего не выполняется, добавьте --apply)"

for item in "${ITEMS[@]}"; do
  IFS='|' read -r table expr <<< "$item"   # разбор через read: подстановка ${x%%|*} роняет bash 5.3 на macOS
  stmt="ALTER TABLE ${table} MODIFY TTL ${expr}"
  if [ "$MODE" = "--apply" ]; then
    reply="$(curl -sS -m 300 "$CH_URL" --data-binary "$stmt" || true)"
    echo "${table}: ${reply:-ok}"
  else
    echo "$stmt"
  fi
done
