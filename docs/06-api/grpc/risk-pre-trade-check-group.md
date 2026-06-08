# gRPC Method: RiskService/PreTradeCheckGroup

## Status

TODO / planned — proposed schema sketch. Материализуется code-implementer #11
после утверждения risk-service boundaries (ADR-025).

## Purpose

Pre-trade risk check для ComboOrder как группы:

- проверка каждой ноги по notional, position, leverage;
- проверка total notional группы;
- проверка margin impact группы;
- проверка максимального количества ног;
- проверка допустимости atomicity scope (в частности: `strict_atomic` при
  внешних ногах без `venue_native` — reject);
- проверка spread / factor constraints.

Возвращает `RiskDecision`: `ACCEPT | REJECT | RESIZE | HALT` + список
нарушенных ограничений.

## Transport

gRPC (вызывается синхронно из `order_flow` перед сохранением `ComboOrder`).

## Service

`fob.risk.v1.RiskService` (planned extension)

## Method (planned)

```proto
rpc PreTradeCheckGroup(PreTradeCheckGroupRequest) returns (PreTradeCheckGroupResponse);
```

## Proposed Schema Sketch

```proto
message PreTradeCheckGroupRequest {
  fob.common.v1.EventMeta meta       = 1;
  string user_id                     = 2;
  string account_id                  = 3;
  // Сериализованный ComboOrder или inline поля.
  fob.orders.v1.ComboOrder combo     = 4;
}

message PreTradeCheckGroupResponse {
  fob.common.v1.EventMeta meta       = 1;
  // ACCEPT | REJECT | RESIZE | HALT
  string decision                    = 2;
  repeated string violated_rules     = 3;
  string reason                      = 4;
  fob.common.v1.Error error         = 5;
}
```

## Idempotency

Идемпотентен по `meta.correlation_id` (read-only side-effects; reject/halt
может иметь state в risk service — явная идемпотентность на уровне risk snapshots).

## Used In Features

- [F-09](../../02-system/features/F-09-batch-combo-orders/)
- [F-07](../../02-system/features/F-07-pretrade-risk-control/)

## Source Fragments

- IN-011 §7 UC-F09-001 шаг 4, §12.4 (Risk Manager responsibilities)
```

---
