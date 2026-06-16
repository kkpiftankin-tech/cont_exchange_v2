---
id: DATA-batch-results-unified
title: ClickHouse view batch_results_unified (+ combo_groups_pg)
level: sea
feature: F-09
store: clickhouse
related: [ADR-042]
---

# batch_results_unified — единый read-слой батчей

Единый источник для отображения батчей (одномерных + многоногих combo). См.
[ADR-042](../03-architecture/adr/ADR-042-unified-batch-read-layer.md).

## combo_groups_pg (ClickHouse table, PostgreSQL engine)

Федерация живых combo-групп из PostgreSQL `execution_groups` (authoritative OLTP).
Не хранит данные — читает PG на каждый запрос.

| Колонка | Тип | Из PG |
| --- | --- | --- |
| batch_id | String | execution_groups.batch_id |
| parent_order_id | String | parent_order_id |
| group_status | String | group_status |
| execution_scale | Decimal128(18) | execution_scale |
| ratio_deviation_bps | Nullable(Int32) | ratio_deviation_bps |
| leg_results | String (JSON) | leg_results |
| created_at | DateTime64(6) | created_at |

```sql
ENGINE = PostgreSQL('postgres:5432', 'cex', 'execution_groups', 'cex', 'cex')
```

## batch_results_unified (ClickHouse VIEW)

UNION ALL двух источников с дискриминатором `kind`:

| Колонка | single_leg (batchresults) | combo_group (combo_groups_pg) |
| --- | --- | --- |
| batch_id | batch_id | batch_id |
| event_time_ms | event_time_ms | toUnixTimestamp64Milli(created_at) |
| status | из residual_norm (>0.1 FAILED, >0.01 PARTIAL, иначе SUCCESS) | из group_status (filled→SUCCESS, failed/rejected/cancelled→FAILED, иначе PARTIAL) |
| solve_time_ms | solve_time_ms | 0 |
| residual_norm | residual_norm | 0 |
| num_active_orders | num_active_orders | JSONLength(leg_results) |
| fills_count | fills_count | кол-во ног с execQty>0 |
| kind | `'single_leg'` | `'combo_group'` |
| parent_order_id | `''` | parent_order_id |
| execution_scale | `''` | toString(execution_scale) |
| ratio_deviation_bps | NULL | ratio_deviation_bps |

## Потребители

- BFF `GET /api/batches` → `fetchBatchesFromClickHouse` (читает только этот view).
- Профиль → вкладка «Батчи» (combo помечены бейджем `combo`).

## Замечания

- combo-строки авторитетны в PostgreSQL `execution_groups` (мутабельное
  состояние); в ClickHouse не дублируются.
- single_leg-строки иммутабельны в ClickHouse `batchresults` (F-04 поток).
- DDL: `infra/clickhouse/init.sql`.
