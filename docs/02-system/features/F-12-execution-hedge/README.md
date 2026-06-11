# F-12 — Execution Hedge

> **Статус:** docs-complete, implementation-partial. Контракты proto/openapi определены, код частично реализован (matching emits ExecutionIntent, venues исполняет через симулятор и реальные адаптеры). Persistence (PostgreSQL hedgeflows / child_orders + ClickHouse execution_reports), Execution Planning routing logic, PreHedgeCheck RPC и HedgePnL calculation — отсутствуют.

## 🧭 Navigation Map (IN-013 drill-down)

Эта секция — **карта документации сверху вниз** для фичи.
Каждый уровень имеет свой ответ на «что/как», и каждая ссылка
ведёт на следующий уровень детализации.

```text
   ┌─ Уровень ──────────────┬─ Артефакт ─────────────────────────────────┐
☁️ L0 │ Что система делает    │ Эта страница + L0 system sequence(s) ниже  │
🌊 L1 │ Какие функции у фичи?  │ Use Cases (таблица ниже)                   │
   │ Какие сервисы участвуют?│ L1 service sequences (per-UC)              │
🐟 L2 │ Из каких классов       │ Component overviews + L2 sequences         │
   │ состоит сервис?        │                                            │
💻 src │ Код                    │ cpp/<component>/src/...                    │
   └────────────────────────┴────────────────────────────────────────────┘
```

## 📋 Use Cases (L1 🌊)

| UC | Имя | L0 sequence ☁️ | L1 sequence 🌊 |
| --- | --- | --- | --- |
| [UC-F12-01](../../use-cases/UC-F12-01-auto-hedge-after-batch/use-case.md) | Auto Hedge After Batch | [SEQ-UC-F12-01-system](../../use-cases/UC-F12-01-auto-hedge-after-batch/sequences/SEQ-UC-F12-01-system.md) | [SEQ-F12-01-auto-hedge-services](../../../05-components/sequences/SEQ-F12-01-auto-hedge-services.md) |
| [UC-F12-02](../../use-cases/UC-F12-02-manual-operator-hedge/use-case.md) | Manual Operator Hedge | [SEQ-UC-F12-02-system](../../use-cases/UC-F12-02-manual-operator-hedge/sequences/SEQ-UC-F12-02-system.md) | [SEQ-F12-02-rejection-fallback-services](../../../05-components/sequences/SEQ-F12-02-rejection-fallback-services.md) |
| [UC-F12-03](../../use-cases/UC-F12-03-partial-fill-retry/use-case.md) | Partial Fill Retry | [SEQ-UC-F12-03-system](../../use-cases/UC-F12-03-partial-fill-retry/sequences/SEQ-UC-F12-03-system.md) | [SEQ-F12-03-error-scenarios-services](../../../05-components/sequences/SEQ-F12-03-error-scenarios-services.md) |
| [UC-F12-04](../../use-cases/UC-F12-04-rejection-fallback/use-case.md) | Rejection Fallback | [SEQ-UC-F12-04-system](../../use-cases/UC-F12-04-rejection-fallback/sequences/SEQ-UC-F12-04-system.md) | — |
| [UC-F12-05](../../use-cases/UC-F12-05-timeout-underfilled-reconciliation/use-case.md) | Timeout Underfilled Reconciliation | [SEQ-UC-F12-05-system](../../use-cases/UC-F12-05-timeout-underfilled-reconciliation/sequences/SEQ-UC-F12-05-system.md) | — |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| [venue-execution-adapter](../../../05-components/venue-execution-adapter/overview.md) | [SEQ-EXEC-ADAPT-001-intent-to-report](../../../05-components/venue-execution-adapter/sequences/SEQ-EXEC-ADAPT-001-intent-to-report.md) |
| [execution-planning](../../../05-components/execution-planning/overview.md) | (L2 sequences pending) |
| [matching-fob-core](../../../05-components/matching-fob-core/overview.md) | [SEQ-MATCHING-001-solver-cycle](../../../05-components/matching-fob-core/sequences/SEQ-MATCHING-001-solver-cycle.md) |
| [risk-manager](../../../05-components/risk-manager/overview.md) | (L2 sequences pending) |
| [external-venues](../../../05-components/external-venues/overview.md) | (L2 sequences pending) |
| [ledger](../../../05-components/ledger/overview.md) | (L2 sequences pending) |
| [observability-reporting](../../../05-components/observability-reporting/overview.md) | (L2 sequences pending) |
| [market-data](../../../05-components/market-data/overview.md) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

## Описание

Автоматическое хеджирование нетто-позиций провайдера на внешних площадках (CEX/DEX/AMM) после внутреннего batch clearing.

Цепочка:

$$
\text{netPosition} \rightarrow \text{ExecutionIntent} \rightarrow \text{child orders} \rightarrow \text{ExecutionReport} \rightarrow \text{HedgePnL update}
$$

После каждого батча Matching Backend (F-04) вычисляет нетто-позицию провайдера по каждому инструменту. Если `|netQty| >= hedgeTriggerThreshold[symbol]` или `triggerNotional >= thresholdNotional`, формируется `ExecutionIntent` и публикуется в Kafka `execution.intents`. Execution Planning подписан на этот топик и на `venue.liquidity.fob` + `venue.health` от F-11; формирует routing plan (split объёма между venue по доле ликвидности), вызывает Risk Manager `PreHedgeCheck`, затем направляет на Venue Execution Adapter, который создаёт HedgeFlow в PostgreSQL и набор child_orders. External Venues Connector (CEX REST/WS, DEX RPC, AMM, VenueSim для backtest) исполняет child orders на реальных площадках, возвращает raw execution events, Adapter нормализует их в ExecutionReport и публикует в Kafka `execution.venue`. Settlement Ledger потребляет execution.venue и обновляет позиции + считает HedgePnL; ClickHouse складывает execution_reports для исторической аналитики и parity-проверок.

## Ключевые сущности

- **ExecutionIntent** — Kafka-сообщение `execution.intents`. Proto: [`fob.execution.v1.ExecutionIntent`](../../../../contracts/proto/fob/execution/v1/execution.proto). Поля: `hedge_flow_id`, `batch_id`, `provider_id`, `instrument`, `side`, `target_qty`, `target_notional`, `reference_mid`, `urgency` (LOW/MEDIUM/HIGH), `allowed_venues`, `timeout_ms`, `client_order_id`, `source` (AUTO_BATCH / MANUAL_OVERRIDE / BACKTEST).
- **HedgeFlow** — сессия хеджа одного ExecutionIntent. PostgreSQL `hedgeflows`. Proto: [`fob.hedge.v1.HedgeFlow`](../../../../contracts/proto/fob/hedge/v1/hedge.proto). Статусы: OPEN, COMPLETED, UNDERFILLED, REJECTED, RISK_REJECTED, CANCELLED.
- **ChildOrder** — конкретный ордер на venue. PostgreSQL `child_orders`. Proto: [`fob.hedge.v1.ChildOrder`](../../../../contracts/proto/fob/hedge/v1/hedge.proto). Статусы: PENDING, FILLED, PARTIALLY_FILLED, CANCELLED, REJECTED.
- **ExecutionReport** — отчёт об исполнении child order. Kafka `execution.venue`. Proto: [`fob.execution.v1.ExecutionReport`](../../../../contracts/proto/fob/execution/v1/execution.proto). Статусы: NEW, PARTIALLY_FILLED, FILLED, CANCELLED, REJECTED, EXPIRED, OVERFILL_GUARD, UNDERFILLED.
- **Urgency** — три уровня c маппингом на ExecutionStrategy и TimeInForce:
  - LOW → POST_ONLY / LIMIT, GTC, target slippage ≤ 5 bps
  - MEDIUM → LIMIT, GTC/IOC, target slippage ≤ 15 bps
  - HIGH → MARKET / LIMIT, IOC, target slippage ≤ 30 bps
- **HedgePnL** — реализованный P&L хеджа. Формула в [04-domain/business-rules.md](../../../04-domain/business-rules.md#hedgepnl).

## Use Cases

- [UC-F12-01 — Auto Hedge After Batch](../../use-cases/UC-F12-01-auto-hedge-after-batch/) — основной auto-flow.
- [UC-F12-02 — Manual Operator Hedge](../../use-cases/UC-F12-02-manual-operator-hedge/) — ручной запуск через Admin UI.
- [UC-F12-03 — Partial Fill Retry](../../use-cases/UC-F12-03-partial-fill-retry/) — частичное исполнение + retry на остаток.
- [UC-F12-04 — Rejection Fallback](../../use-cases/UC-F12-04-rejection-fallback/) — отказ venue A → перенаправление на venue B.
- [UC-F12-05 — Timeout / Underfilled Reconciliation](../../use-cases/UC-F12-05-timeout-underfilled-reconciliation/) — таймаут + reconciliation gap.

## Sequence Diagrams

- System-level: [SEQ-UC-F12-01-system](../../use-cases/UC-F12-01-auto-hedge-after-batch/sequences/SEQ-UC-F12-01-system.md), [SEQ-UC-F12-02-system](../../use-cases/UC-F12-02-manual-operator-hedge/sequences/SEQ-UC-F12-02-system.md), [SEQ-UC-F12-03-system](../../use-cases/UC-F12-03-partial-fill-retry/sequences/SEQ-UC-F12-03-system.md), [SEQ-UC-F12-04-system](../../use-cases/UC-F12-04-rejection-fallback/sequences/SEQ-UC-F12-04-system.md), [SEQ-UC-F12-05-system](../../use-cases/UC-F12-05-timeout-underfilled-reconciliation/sequences/SEQ-UC-F12-05-system.md).
- Service-level (cross-component): [SEQ-F12-01-auto-hedge-services](../../../05-components/sequences/SEQ-F12-01-auto-hedge-services.md), [SEQ-F12-02-rejection-fallback-services](../../../05-components/sequences/SEQ-F12-02-rejection-fallback-services.md), [SEQ-F12-03-error-scenarios-services](../../../05-components/sequences/SEQ-F12-03-error-scenarios-services.md).

## Контракты

- Kafka: [execution.intents](../../../06-api/messaging/execution-intents.md), [execution.venue](../../../06-api/messaging/execution-venue.md), [risk.alerts](../../../06-api/messaging/risk-alerts.md).
- gRPC: [risk-pre-hedge-check](../../../06-api/grpc/risk-pre-hedge-check.md), [ledger-apply-execution-report](../../../06-api/grpc/ledger-apply-execution-report.md).
- REST: [hedgeflows](../../../06-api/rest/hedgeflows.md) — операторская панель + UI.
- Proto: [`fob.execution.v1`](../../../../contracts/proto/fob/execution/v1/execution.proto), [`fob.hedge.v1`](../../../../contracts/proto/fob/hedge/v1/hedge.proto), [`fob.venue.v1`](../../../../contracts/proto/fob/venue/v1/venue.proto).

## Данные

- [`hedgeflows`](../../../07-data/hedgeflows.md) — PostgreSQL OLTP.
- [`child_orders`](../../../07-data/child-orders.md) — PostgreSQL OLTP.
- [`execution_reports`](../../../07-data/execution-reports.md) — ClickHouse OLAP.

## Реализация

- matching:
  - [hedge_trigger_policy.cpp](../../../../cpp/matching/src/app/hedge_trigger_policy.cpp) — порог `|netQty|` / `triggerNotional`.
  - [execution_intent_builder.cpp](../../../../cpp/matching/src/app/execution_intent_builder.cpp) — формирование ExecutionIntent.
  - [hedge_execution_intents_publisher.cpp](../../../../cpp/matching/src/app/hedge_execution_intents_publisher.cpp) — publish в Kafka.
  - [external_venue_filter.cpp](../../../../cpp/matching/src/app/external_venue_filter.cpp) — фильтр allowed venues.
  - [position_snapshot_calculator.cpp](../../../../cpp/matching/src/app/position_snapshot_calculator.cpp) — netQty из BatchResult.
  - [execution_planning_uc.hpp](../../../../cpp/matching/src/app/execution_planning_uc.hpp) — интерфейс (stub).
- venues (хостит Venue Execution Adapter + EVC):
  - [execute_on_venue.cpp](../../../../cpp/venues/src/app/execute_on_venue.cpp) — исполнение child order через VenueAdapter.
  - [execution_intents_consumer.cpp](../../../../cpp/venues/src/infra/execution_intents_consumer.cpp) — Kafka consumer.
  - [execution_report_producer.cpp](../../../../cpp/venues/src/infra/execution_report_producer.cpp) — Kafka producer.
  - [cex_ws_rest_adapter.cpp](../../../../cpp/venues/src/infra/cex_ws_rest_adapter.cpp) — CEX REST/WS.
  - [dex_amm_rpc_adapter.cpp](../../../../cpp/venues/src/infra/dex_amm_rpc_adapter.cpp) — DEX/AMM RPC.
  - [venue_sim_adapter.cpp](../../../../cpp/venues/src/infra/venue_sim_adapter.cpp) — backtest VenueSim.

## Acceptance criteria

См. [acceptance-criteria.md](acceptance-criteria.md) — F12-1..F12-12, F12-N1..F12-N8, U1-U10, IT-1..IT-7.

## Известные несоответствия спецификации

См. [feature.yaml](feature.yaml#knownIssues). Кратко:

1. Нет отдельного сервиса `venue_execution_adapter` (объединён с `cpp/venues/`). Требует ADR.
2. Нет реализации `execution_planning` (routing plan по `venue.liquidity.fob`).
3. PostgreSQL `hedgeflows` и `child_orders` отсутствуют в `infra/postgres/init.sql`.
4. ClickHouse `execution_reports` отсутствует в `infra/clickhouse/init.sql`.
5. `RiskService.PreHedgeCheck` RPC не определён в proto.
6. `LedgerService.ApplyExecutionReport` не вычисляет HedgePnL.
7. Дублирование топиков `execution.venue` vs `execution.reports` (legacy).

## Связанные фичи

- F-04 (Batch Clearing) — поставщик clearing price и netQty.
- F-06 (Positions / PnL / Margin) — потребитель ExecutionReport для обновления позиций.
- F-07 (Pre-trade Risk) — расширяется методом `PreHedgeCheck` для F12-8.
- F-08 (Post-trade Risk / Liquidations) — мониторинг hedgeExposure post-trade.
- F-11 (External Venues LOB → FOB) — поставщик `venue.liquidity.fob` + `venue.health` для routing plan.
- F-15 (Backtest / Replay) — parity tests через VenueSim.

## Traceability

См. [traceability.yaml](traceability.yaml).

## Источник спецификации

- IN-005: [`incoming-docs/2026-05-20-F-12-execution-hedge-v1.md`](../../../../incoming-docs/2026-05-20-F-12-execution-hedge-v1.md).
- Fragment map: [`incoming-docs/IN-005.fragment-map.md`](../../../../incoming-docs/IN-005.fragment-map.md).

## Source Fragments

- IN-001-FR-027, IN-001-FR-028 (feature baseline)
- IN-005 §1 (термины), §2-4 (sequence diagrams), §5 (reconciliation), §6 (формулы расчётов), §7 (AC F12-1..F12-12), §8 (NFR), §9 (REST + Kafka), §10 (тесты U1-U10, IT-1..IT-7), §11 (DoD)
