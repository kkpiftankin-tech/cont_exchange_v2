# gRPC Method: RiskService/PreHedgeCheck

## Status

planned (proto extension required; см. [open-questions.md §10](../../02-system/features/F-12-execution-hedge/open-questions.md)).

## Purpose

Pre-hedge risk check для `ExecutionIntent` перед размещением child orders на venues. Три проверки (IN-005 §6 / F12-8):

1. $\text{targetNotional} \leq \text{maxNotionalPerHedge}$
2. $\text{currentHedgeExposure}[\text{symbol}] + \text{targetQty} \leq \text{hedgeExposureLimit}[\text{symbol}]$
3. $\text{expectedSlippage} \leq \text{maxSlippage}[\text{urgency}]$

При REJECT — Execution Planning не размещает child orders; HedgeFlow помечается `status=RISK_REJECTED`; publish `risk.alerts(type=HEDGE_REJECTED, reason)`.

## Transport

gRPC

## Service

`fob.risk.v1.RiskService`

## Method (planned proto extension)

```proto
service RiskService {
  // existing
  rpc CheckNewOrder(CheckNewOrderRequest) returns (CheckNewOrderResponse);
  rpc GetRiskSnapshot(GetRiskSnapshotRequest) returns (GetRiskSnapshotResponse);
  rpc SetKillSwitch(SetKillSwitchRequest) returns (SetKillSwitchResponse);
  rpc OnBatchResult(OnBatchResultRequest) returns (OnBatchResultResponse);

  // new — F-12
  rpc PreHedgeCheck(PreHedgeCheckRequest) returns (PreHedgeCheckResponse);
}

message PreHedgeCheckRequest {
  fob.common.v1.EventMeta meta = 1;
  fob.execution.v1.ExecutionIntent intent = 2;
  fob.common.v1.Decimal expected_slippage_bps = 3;  // оценка из routing plan
  fob.common.v1.Decimal current_hedge_exposure = 4; // текущая |exposure| по symbol
}

message PreHedgeCheckResponse {
  fob.common.v1.EventMeta meta = 1;
  bool ok = 2;
  string reject_reason = 3;  // NOTIONAL_LIMIT / EXPOSURE_LIMIT / SLIPPAGE_LIMIT
  PreHedgeCheckDetails details = 4;
  fob.common.v1.Error error = 5;
}

message PreHedgeCheckDetails {
  // notional check
  fob.common.v1.Decimal target_notional = 1;
  fob.common.v1.Decimal max_notional_per_hedge = 2;

  // exposure check
  fob.common.v1.Decimal current_hedge_exposure = 10;
  fob.common.v1.Decimal target_qty = 11;
  fob.common.v1.Decimal hedge_exposure_limit = 12;

  // slippage check
  fob.common.v1.Decimal expected_slippage_bps = 20;
  fob.common.v1.Decimal max_slippage_bps = 21;  // зависит от urgency
}
```

## Caller

- [execution-planning](../../05-components/execution-planning/overview.md) — primary caller; вызывает перед передачей в Adapter.
- [gateway](../../05-components/gateway/overview.md) — для POST `/hedge/intents/manual` (sync REST → gRPC check → 422 если REJECT).

## Callee

- [risk-manager](../../05-components/risk-manager/overview.md)

## Idempotency

Метод чистый (read-only по state risk-manager + state hedge exposure). Повторный вызов с теми же параметрами должен давать тот же результат при неизменной risk policy и текущей exposure.

## SLA

- p95 latency: ≤ 50 ms (входит в overall F12-10 budget ≤ 100 ms CEX).
- error rate: < 0.1%.

## Used In Features

- [F-12. Execution Hedge](../../02-system/features/F-12-execution-hedge/) — primary.
- [F-07. Pre-trade Risk](../../02-system/features/F-07-pretrade-risk/) — расширение (новый метод сервиса).

## Used In Use Cases

- [UC-F12-01](../../02-system/use-cases/UC-F12-01-auto-hedge-after-batch/use-case.md)
- [UC-F12-02](../../02-system/use-cases/UC-F12-02-manual-operator-hedge/use-case.md)
- [UC-F12-03](../../02-system/use-cases/UC-F12-03-partial-fill-retry/use-case.md) (для retry intent)
- [UC-F12-04](../../02-system/use-cases/UC-F12-04-rejection-fallback/use-case.md) (для fallback intent)

## Used In Sequence Diagrams

- [SEQ-F12-01-auto-hedge-services](../../05-components/sequences/SEQ-F12-01-auto-hedge-services.md)
- [SEQ-F12-02-rejection-fallback-services](../../05-components/sequences/SEQ-F12-02-rejection-fallback-services.md)
- [SEQ-F12-03-error-scenarios-services](../../05-components/sequences/SEQ-F12-03-error-scenarios-services.md)

## Related Components

- [risk-manager](../../05-components/risk-manager/overview.md)
- [execution-planning](../../05-components/execution-planning/overview.md)

## Related Data Objects

- `risk_limits` (PostgreSQL) — `max_notional_per_hedge`, `hedge_exposure_limit[symbol]`, `max_slippage_bps[urgency]`.
- `risk_snapshots` (PostgreSQL) — текущая `current_hedge_exposure`.

## Source Fragments

- IN-005 §6 «Pre-hedge Risk Check (×3)»
- IN-005 §7 F12-8
- IN-005 §10.1 U10 «Pre-hedge risk check rejection»
