# gRPC Method: RiskService/SetKillSwitch

## Status

draft (proto-defined)

## Purpose

Operator-команда: активировать/деактивировать kill-switch — глобальный или per-instrument. После активации все новые заявки получают `RiskDecision.HALT`, существующие активные заявки могут быть отменены или переведены в `paused` по policy.

В сервисных диаграммах F-16 упоминаются `ActivateKillSwitch` и `DeactivateKillSwitch` — это два режима одного proto-метода (`halt=true` / `halt=false`).

## Transport

gRPC

## Service

`fob.risk.v1.RiskService`

## Method

```proto
rpc SetKillSwitch(KillSwitchRequest) returns (KillSwitchResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (через operator-console UI; требует операторской авторизации)

## Callee

- [risk-manager](../../05-components/risk-manager/overview.md)

## Schema

См. [contracts/proto/fob/risk/v1/risk.proto](../../../contracts/proto/fob/risk/v1/risk.proto).

```proto
message KillSwitchRequest {
  fob.common.v1.EventMeta meta = 1;
  string instrument_symbol = 2;  // empty => global
  bool halt = 3;
  string reason = 4;
}

message KillSwitchResponse {
  fob.common.v1.EventMeta meta = 1;
  bool effective_halt = 2;
}
```

## Security

Требует operator-роли. Каждый вызов должен попадать в audit-log с `meta.correlation_id`, `actor`, `reason`.

## Used In Features

- [F-16. Operator Console](../../02-system/features/F-16-operator-console/)
- (косвенно) [F-07. Pre-Trade Risk](../../02-system/features/F-07-pretrade-risk/) — последствия halt

## Used In Use Cases

- [UC-F16-01. Activate Kill Switch](../../02-system/use-cases/UC-F16-01-trigger-kill-switch/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F16-UC-F16-01-services](../../05-components/sequences/SEQ-F16-UC-F16-01-services.md)

## Related Components

- [risk-manager](../../05-components/risk-manager/overview.md)
- [gateway](../../05-components/gateway/overview.md)
- [observability-reporting](../../05-components/observability-reporting/overview.md) (consumer `risk.alerts`)

## Related Data Objects

- Kafka `risk.alerts` (event `KILL_SWITCH`)
- `risk_events` (ClickHouse, история операторских действий)

## Source Fragments

- IN-001-FR-016 (FR-RISK-003 kill-switch)
- IN-001-FR-022
