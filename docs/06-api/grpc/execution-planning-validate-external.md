# gRPC Method: ExecutionPlanningService/ValidateExternalLegExecution

## Status

**Deferred — superseded for MVP by [ADR-037](../../03-architecture/adr/ADR-037-external-leg-execution-compensation.md).**
Отдельный `ExecutionPlanningService` не реализован. Валидация atomicityScope
(INTERNAL_BATCH запрещает внешние ноги; EXTERNAL_COMPENSATING → best-effort +
компенсация) в MVP enforced инлайн: order_flow `GroupPreTradeCheck`
(см. [risk-pre-trade-check-group.md](risk-pre-trade-check-group.md), ComboPolicy
flags/limits) + matching grouped solver (`grouped_solver_bisection`,
EXTERNAL_COMPENSATING ⇒ внутренние ноги kBlocked без counter-liquidity). Внешние
ноги маршрутизируются напрямую (`cpp/matching/src/app/combo_external_routing.cpp`
→ `execution.intents` → venues), без отдельного planning-сервиса. Схема ниже
сохранена как design-эскиз на случай будущего выделения сервиса.

## Purpose

Проверить допустимость внешнего исполнения ног с учётом текущей политики атомарности.

Правило (ADR-031, BR-F09-008):
- если `atomicityScope = INTERNAL_BATCH` — внешние ноги запрещены;
- если `atomicityScope = VENUE_NATIVE` — допустимы только ноги на venue с native
  atomic multi-leg support;
- если `atomicityScope = EXTERNAL_COMPENSATING` — внешние ноги в best-effort режиме,
  система запускает compensating action при частичном результате.

## Transport

gRPC

## Service

`fob.execution.v1.ExecutionPlanningService` (planned)

## Method (planned)

```proto
rpc ValidateExternalLegExecution(ValidateExternalLegExecutionRequest)
    returns (ValidateExternalLegExecutionResponse);
```

## Proposed Schema Sketch

```proto
message ValidateExternalLegExecutionRequest {
  fob.common.v1.EventMeta meta              = 1;
  string parent_order_id                     = 2;
  fob.orders.v1.AtomicityPolicy policy      = 3;
  fob.orders.v1.AtomicityScope scope        = 4;
  repeated string external_leg_ids           = 5;
  repeated string target_venues              = 6;
}

message ValidateExternalLegExecutionResponse {
  fob.common.v1.EventMeta meta              = 1;
  // Допустима ли внешняя маршрутизация при данной политике.
  bool permitted                             = 2;
  // Причина запрета.
  string denial_reason                       = 3;
  // Может ли заявляться strict_atomic при данных venue.
  bool strict_atomic_possible                = 4;
  fob.common.v1.Error error                 = 5;
}
```

## Used In Features

- [F-09](../../02-system/features/F-09-batch-combo-orders/)

## Source Fragments

- IN-011 §7 UC-F09-003, §10.5, BR-F09-008
```

---
