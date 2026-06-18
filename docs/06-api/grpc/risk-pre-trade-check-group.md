# gRPC Method: RiskService/PreTradeCheckGroup

## Status

✅ **Реализовано (T-F09-040).** `rpc PreTradeCheckGroup` в `fob.risk.v1.RiskService`.
Доменная логика — `cpp/risk/src/domain/grouped_risk_check.cpp` (pure,
`GroupPreTradeCheck`): max legs, total notional ≤ limit, max external legs,
`strict_atomic + external_compensating → REJECT` (AC-F09-006). Лимиты — из env
(`F09_MAX_LEGS_PER_GROUP`, `F09_MAX_EXTERNAL_LEGS`, `F09_MAX_TOTAL_NOTIONAL`).
order_flow `CreateComboOrderUseCase` вызывает метод через `RiskClient`
(fail-closed). MVP: notional ноги оценивается как `q_max * p_high`; `external`
всегда false (внешние ноги — MVP-5); spread/factor — MVP-3.

Фактический контракт (`contracts/proto/fob/risk/v1/risk.proto`):

```proto
message GroupRiskLegInput {
  string instrument_symbol = 1;
  fob.common.v1.Decimal notional = 2;   // |qty * price| quote
  bool external = 3;
}
message PreTradeCheckGroupRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  string atomicity_policy = 3;   // strict_atomic/scalable_atomic/best_effort/...
  string atomicity_scope = 4;    // internal_batch/venue_native/...
  repeated GroupRiskLegInput legs = 5;
}
message PreTradeCheckGroupResponse {
  fob.common.v1.EventMeta meta = 1;
  RiskDecision decision = 2;     // ACCEPT/REJECT
  fob.common.v1.Error error = 3; // reason на REJECT (code=GROUPED_PRE_TRADE_REJECTED)
}
rpc PreTradeCheckGroup(PreTradeCheckGroupRequest) returns (PreTradeCheckGroupResponse);
```

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
- [F-07](../../02-system/features/F-07-pretrade-risk/)

## Source Fragments

- IN-011 §7 UC-F09-001 шаг 4, §12.4 (Risk Manager responsibilities)
```

---
