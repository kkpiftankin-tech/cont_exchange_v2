# Topic: execution.groups

| Свойство | Значение |
|---|---|
| Producer | `matching` (Matching Backend) |
| Consumers | `ledger`, `risk` (post-trade), `order_flow` (parent/leg status update), `observability`, `market_data` (ClickHouse `grouped_*` OLAP sink), UI stream, `backtest` (replay) |
| Message type | `fob.matching.v1.ExecutionGroup` |
| Partition key | `parentOrderId` |
| Delivery | at-least-once + idempotent consumer (по `executionGroupId`) |
| Retention | 7d (604 800 000 ms) |
| Создаётся | `infra/kafka/create_topics.sh` (добавляет devops #14) |

## Назначение

Канонический envelope результата grouped execution в одном batch cycle.
Содержит согласованный вектор исполнений (`ExecutionGroup`) и позволяет
consumers применить изменения атомарно по группе.

Ключевой инвариант (ADR-033): `LegFill` в топике `fills` фиксируется
не раньше или одновременно с публикацией соответствующего `ExecutionGroup`
в `execution.groups`. Consumer-ы `ledger` и `risk` должны ждать
`ExecutionGroup` перед применением `LegFill`.

Топик изолирован от `batch.outputs` (ADR-033): разные partition key
(`parentOrderId` vs `batch_id`), разные сущности, разные idempotency-ключи.

## Схема сообщения

Proto-файл: `contracts/proto/fob/matching/v1/execution_group.proto`
(создаёт code-implementer #11).

### Enums

```proto
enum GroupStatus {
  GROUP_STATUS_UNSPECIFIED         = 0;
  GROUP_STATUS_FILLED              = 1;
  GROUP_STATUS_PARTIAL             = 2;
  GROUP_STATUS_WAITING_NEXT_BATCH  = 3;
  GROUP_STATUS_CANCELLED_BY_ATOMICITY = 4;
  GROUP_STATUS_DEGRADED            = 5;
  GROUP_STATUS_COMPENSATING        = 6;
  GROUP_STATUS_ROLLBACK_PENDING    = 7;
  GROUP_STATUS_ROLLEDBACK          = 8;
  GROUP_STATUS_FAILED              = 9;
}
```

### GroupSolverDiagnostics

```proto
// Диагностика группового решателя (аналог BatchSolverDiagnostics для группы).
// double допустим для solver diagnostics (CLAUDE.md §9).
message GroupSolverDiagnostics {
  // Время решения групповой задачи, мс.
  uint32 group_solve_time_ms       = 1;
  // Leg-ids, ограничившие общий масштаб (binding legs).
  repeated string binding_leg_ids  = 2;
  // Constraint-ids, ставшие активными при решении (binding constraints).
  repeated string binding_constraint_ids = 3;
  // Остаточная норма по группе (diagnostic only).
  double group_residual_norm       = 4;
  // JSON-дамп внутренних параметров решателя.
  string solver_params_json        = 5;
}
```

### LegResult

```proto
// Результат исполнения одной ноги в составе ExecutionGroup.
message LegResult {
  string leg_id                          = 1;
  // Исполненный объём ноги (base units).
  fob.common.v1.Decimal exec_qty         = 2;
  // Цена исполнения (quote per base). Decimal, не double.
  fob.common.v1.Decimal exec_price       = 3;
  // Исполненный номинал (exec_qty * exec_price) в quote currency.
  fob.common.v1.Decimal exec_notional    = 4;
  // Ссылка на fill_id в топике fills.
  string fill_id                         = 5;
  // Источник ликвидности (internal / cex_hedge / dex_hedge / ...).
  string liquidity_source                = 6;
  // T-F09-062 (additive): инструмент и сторона ноги — нужны ledger для
  // постинга в позицию (знак позиции по side).
  string instrument_symbol               = 7;
  fob.common.v1.Side side                = 8;
}
```

### ExecutionGroup (главное сообщение топика)

```proto
// Kafka envelope топика execution.groups.
// Partition key = parentOrderId.
message ExecutionGroup {
  // EventMeta обязателен (CLAUDE.md §7, partition_key = parentOrderId).
  fob.common.v1.EventMeta meta              = 1;

  // Уникальный идентификатор группы исполнения (idempotency key для consumers).
  string execution_group_id                 = 2;

  // Идентификатор batch cycle (F-04), в котором произошло исполнение.
  string batch_id                           = 3;

  // Идентификатор родительской ComboOrder или BatchOrder.
  string parent_order_id                    = 4;

  // Режим исполнения (из combo.proto).
  ExecutionMode execution_mode              = 5;

  // Политика атомарности (из combo.proto).
  AtomicityPolicy atomicity_policy          = 6;

  // Область атомарности (из combo.proto).
  AtomicityScope atomicity_scope            = 7;

  // Статус группы.
  GroupStatus group_status                  = 8;

  // Масштаб исполнения группы (0.0–1.0).
  // Decimal для финансовой точности.
  fob.common.v1.Decimal execution_scale     = 9;

  // Фактическое отклонение соотношения, в базисных пунктах.
  uint32 ratio_deviation_bps                = 10;

  // Результаты по ногам.
  repeated LegResult leg_results            = 11;

  // Нарушенные ограничения (constraint_id-ы).
  repeated string violated_constraints      = 12;

  // Применённое резервное действие (пусто если не применялось).
  string fallback_action                    = 13;

  // Диагностика решателя.
  GroupSolverDiagnostics solver_diagnostics = 14;

  google.protobuf.Timestamp created_at      = 15;

  // T-F09-062 (additive): владелец заявки — нужен ledger для постинга и
  // hierarchical PnL (leg/group/parent/strategy).
  string user_id                            = 16;
}
```

### LegFill (дополнение к топику fills)

`LegFill` публикуется в существующий топик `fills` (не в `execution.groups`).
В полях `fob.matching.v1.FlowFill` добавляются (в новом файле или расширением)
следующие поля для combo-контекста:

```proto
// Расширение FlowFill для combo-ног — добавляется к существующей структуре
// в batch.proto (новые теги, backward-compatible).
// ВНИМАНИЕ: не менять существующие теги 1–10 в FlowFill.
// Эти поля добавляет code-implementer #11.

// Тег 20+ зарезервирован для F-09 combo-расширений FlowFill:
// string parent_order_id   = 20;
// string execution_group_id = 21;
// string leg_id            = 22;
// string group_policy      = 23;  // AtomicityPolicy строкой для читаемости
// string liquidity_source  = 9;   // уже есть в FlowFill; для LegFill переиспользуется
```

Расширение `FlowFill` новыми тегами 20+ — backward-compatible, существующие
consumers игнорируют неизвестные поля. Code-implementer #11 согласовывает
точные теги с proto-contract-designer перед материализацией.

## Импорты для execution_group.proto

```proto
syntax = "proto3";
package fob.matching.v1;

import "google/protobuf/timestamp.proto";
import "fob/common/v1/common.proto";
import "fob/orders/v1/combo.proto";  // ExecutionMode, AtomicityPolicy, AtomicityScope
```

## Когда публикуется

Matching Backend публикует одно сообщение `ExecutionGroup` после завершения
grouped solve для каждой активной `ComboOrder` в каждом batch cycle:

- при полном исполнении (`GROUP_STATUS_FILLED`);
- при частичном исполнении (`GROUP_STATUS_PARTIAL`);
- при отмене из-за atomicity (`GROUP_STATUS_CANCELLED_BY_ATOMICITY`);
- при деградации (`GROUP_STATUS_DEGRADED`);
- при компенсации (`GROUP_STATUS_COMPENSATING`, `GROUP_STATUS_ROLLBACK_PENDING`).

Если для `strict_atomic` grouped solve не даёт допустимого результата —
публикуется `ExecutionGroup` со статусом `GROUP_STATUS_CANCELLED_BY_ATOMICITY`
и пустым `leg_results[]`. `LegFill`-ы в `fills` **не публикуются**.

## Когда консумится

| Consumer | Действие |
|---|---|
| `ledger` | Применить leg postings, parent-level grouped summary, fee, PnL, margin; идемпотентно по `execution_group_id` |
| `risk` | Post-trade risk check группы; оповещение при violated_constraints |
| `order_flow` | Обновить parent/leg статусы; запустить child graph transitions (OCO cancel-siblings, bracket resize) |
| `observability` | Запись метрик: grouped solve time, ratio deviation, degraded/orphan incidents |
| `market_data` | OLAP-ingestion в ClickHouse: 1 строка `grouped_execution_events` + N строк `grouped_leg_fills` на группу (ReplacingMergeTree, идемпотентно по `event_time_ms`). Через `ClickHouseBatchStorage::SaveExecutionGroup` |
| UI stream | Обновление статуса группы и ног в интерфейсе |
| `backtest` | Replay grouped execution (AC-F09-010) |

## Idempotency

Consumer обязан дедуплицировать по `execution_group_id`. При повторной доставке:
- `ledger`: проверяет `group_state_transitions` (ADR-032); повторное применение
  не создаёт дублей проводок.
- `order_flow`: проверяет `group_state_transitions`; transition-ы идемпотентны.

## Replay-семантика

- `batch_id` внутри `ExecutionGroup` позволяет F-15 (Backtest Replay) воспроизвести
  grouped execution детерминированно при одном и том же входе (AC-F09-010).
- Топик `backtest` потребляет `execution.groups` с начала (offset reset) при replay-сессии.
- Retention 7d достаточен для rolling replay окна; для long-term audit — архив в ClickHouse.

## Used In Features

- [F-09. Batch/Combo/Multi-leg Orders](../../02-system/features/F-09-batch-combo-orders/)
- [F-15. Backtest / Replay](../../02-system/features/F-15-backtest-replay/)

## Used In Use Cases

- [UC-F09-02. Grouped matching в batch cycle](../../02-system/use-cases/UC-F09-02-grouped-matching/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F09-UC-F09-02-services](../../05-components/sequences/SEQ-F09-UC-F09-02-services.md)

## Related ADR

- [ADR-031](../../03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md)
- [ADR-032](../../03-architecture/adr/ADR-032-parent-child-order-model.md)
- [ADR-033](../../03-architecture/adr/ADR-033-execution-groups-topic.md)

## Related Data Objects

- [oltp-schema.md — execution_groups, group_state_transitions](../../07-data/oltp-schema.md)
- ClickHouse: `grouped_execution_events`, `grouped_leg_fills`, `grouped_quality_metrics`

## Source Fragments

- IN-011 §11.4 (commit rule), §13.2 (топик), §13.3 (fills), §20 (инварианты)
- ADR-033
```

---
