# gRPC Method: ExecutionPlanningService/ValidateExternalLegExecution

## Status

TODO / planned — proposed schema sketch. Материализуется code-implementer #11.

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
