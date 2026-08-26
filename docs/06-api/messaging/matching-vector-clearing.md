---
id: MSG-MATCHING-VECTOR-CLEARING
phase: 06-api
status: draft
owner: matching-team
feature: F-05A
related:
  - contracts/proto/fob/marketdata/v1/vector_liquidity.proto
  - docs/02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md
  - docs/implementation-plan/F-05A-vectorized-external-liquidity.tasks.md
---

# Kafka topic `matching.vector_clearing`

## Назначение

Диагностический поток результатов векторного клиринга F-05A (T-F05A-305, шаг 1a).
`matching` потребляет [`marketdata.vectorized`](marketdata-vectorized.md)
(`VectorClearingInput`), решает QP `Wx=0` (OSQP, ADR-048) + применяет surplus-политику
(ADR-047) и публикует **результат/диагностику** сюда.

**Это НЕ денежный поток.** Он не эмитит `FillEvent`/`ExecutionGroup` и не приводит к
ledger-проводкам. Money-path (эмиссия fills) — отдельный шаг T-F05A-305.

## Контракт

| Поле | Значение |
| --- | --- |
| Payload | [`fob.marketdata.v1.VectorClearingResult`](../../../contracts/proto/fob/marketdata/v1/vector_liquidity.proto) |
| Producer | `matching` (`MatchingLoop`, consumer `marketdata.vectorized`) |
| Consumers | `observability` / `market_data` (persist `vector_clearing_results` в ClickHouse), `backtest` |
| Key | `batch_id` (partition-stability для replay/audit) |
| Delivery | at-least-once; consumer идемпотентен по `batch_id` |
| Retention | short (≈1h) — диагностика; долговременная история — в ClickHouse `vector_clearing_results` |

## Поля результата

- `x` — executed rate на сегмент (`Decimal`, §9);
- `pi` — item prices по активам (заполняется при появлении dual-цен);
- `residual` — `Wx` по активам (диагностика);
- `solver_status` — `CONVERGED` / `DEGRADED` / `FAILED`;
- `surplus` — `SurplusInfo[]` при аллокационной политике (ADR-047);
- `diagnostics` — `residual_norm`, `iterations`, `solve_time_ms`.

## Регистрация

- Топик: [`infra/kafka/create_topics.sh`](../../../infra/kafka/create_topics.sh) (`matching.vector_clearing`).
- Персист истории: ClickHouse `vector_clearing_results` (см. `infra/clickhouse/init.sql`).
