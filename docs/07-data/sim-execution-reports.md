# Data — F-20 sim OLAP (ClickHouse)

Источник: IN-010. Feature: [F-20](../02-system/features/F-20-live-venue-simulator/README.md).
Owner: **observability/venues**. **Документация целевой схемы**; DDL в
`infra/clickhouse/init.sql` — на этапе реализации (не в этой docs-итерации).

## `sim_execution_reports`

История всех синтетических исполнений (`sim_mode=1`). MergeTree, партиции по месяцу,
TTL 90 дней (NFR: хранение ≥ 90 дней).

Ключевые поля: `execution_id`, `sim_session_id`, `hedge_flow_id`, `child_order_id`,
`venue_id`, `symbol`, `side`, `filled_qty`, `avg_price`, `fee`, `status`, `reject_reason`,
`sim_mode UInt8`, `lob_snapshot_id`, `lob_age_ms`, `impact_bps`, `slippage_bps`,
`latency_sample_ms`, `timestamp DateTime64(3)`.

- `ORDER BY (sim_session_id, timestamp, venue_id, symbol)`; `PARTITION BY toYYYYMM(timestamp)`.
- **Writer:** Kafka `execution.venue` → CH connector (см. CN-F20-04 про имя топика).
- **Reader:** Observability (Live Sim Feed, Impact Analysis), отчётность.
- Деньги (`avg_price`, `fee`) — в целевой proto/Decimal; в CH хранятся как Float64 (аналитика, не ledger).

## `sim_divergence_log` (SHADOW)

Попарное сравнение LIVE vs SIM по `client_order_id`. MergeTree, TTL 180 дней.

Ключевые поля: `divergence_id`, `sim_session_id`, `client_order_id`, `hedge_flow_id`,
`venue_id`, `symbol`, `live_filled_qty`/`sim_filled_qty`/`delta_fill_qty`,
`live_avg_price`/`sim_avg_price`/`delta_price_bps`,
`live_latency_ms`/`sim_latency_ms`/`delta_latency_ms`,
`live_fee`/`sim_fee`/`delta_fee`, `timestamp`.

- **Writer:** Divergence Service (F-20, SHADOW). **Reader:** Observability (Sim vs Live), калибровка моделей.

## Links

- Feature [F-20](../02-system/features/F-20-live-venue-simulator/README.md) · UC [F20-02](../02-system/use-cases/UC-F20-02-shadow-compare/use-case.md)
- Messaging: [sim-topics](../06-api/messaging/sim-topics.md) · OLTP: [sim-sessions](sim-sessions.md)
