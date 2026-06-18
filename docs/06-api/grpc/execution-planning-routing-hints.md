# gRPC Method: ExecutionPlanningService/GetGroupedRoutingHints

## Status

**Deferred — design sketch.** Отдельный `ExecutionPlanningService` не реализован.
В MVP feasible caps по ногам считает сам matching (`ComputeFeasibleCaps`:
`min(remaining, q_rate, q_liq, q_risk, q_venue)` с учётом venue liquidity из
`venue.liquidity.fob`), без отдельного planning-сервиса и pre-solve RPC. Схема ниже
сохранена как design-эскиз на случай будущего выделения routing-hints в сервис.
См. [ADR-037](../../03-architecture/adr/ADR-037-external-leg-execution-compensation.md).

## Purpose

Получить grouped routing hints для Matching Backend перед grouped solve:

- expected feasible caps по ногам (с учётом venue liquidity);
- fallback plan при деградации;
- external execution feasibility (venue поддерживает native atomic multi-leg?);
- latency / venue risk hints.

Execution Planning **не подменяет** grouped solver Matching Backend.
Возвращает только hints; финальное решение принимает solver.

## Transport

gRPC (вызывается Matching Backend в batch loop).

## Service

`fob.execution.v1.ExecutionPlanningService` (planned new service)

## Method (planned)

```proto
rpc GetGroupedRoutingHints(GetGroupedRoutingHintsRequest)
    returns (GetGroupedRoutingHintsResponse);
```

## Proposed Schema Sketch

```proto
message LegRoutingHint {
  string leg_id                          = 1;
  // Допустимый объём исполнения ноги с учётом venue liquidity.
  fob.common.v1.Decimal feasible_cap    = 2;
  // Предпочтительный источник ликвидности.
  string preferred_venue                 = 3;
  // True если venue поддерживает native atomic multi-leg.
  bool venue_supports_native_atomic     = 4;
  // Оценка задержки исполнения (мс).
  uint32 estimated_latency_ms            = 5;
}

message GetGroupedRoutingHintsRequest {
  fob.common.v1.EventMeta meta          = 1;
  string batch_id                        = 2;
  string parent_order_id                 = 3;
  fob.orders.v1.ComboOrder combo        = 4;
}

message GetGroupedRoutingHintsResponse {
  fob.common.v1.EventMeta meta          = 1;
  repeated LegRoutingHint leg_hints     = 2;
  // Может ли вся группа исполняться с заявленным atomicity_scope.
  bool group_feasible                   = 3;
  string fallback_recommendation        = 4;
  fob.common.v1.Error error            = 5;
}
```

## Idempotency

Read-only; идемпотентен по definition.

## Used In Features

- [F-09](../../02-system/features/F-09-batch-combo-orders/)
- [F-12](../../02-system/features/F-12-execution-hedge/)

## Source Fragments

- IN-011 §11.3 шаги 5–6, §12.7
```

---
