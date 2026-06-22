# gRPC Method: OrderFlowService/ResolveCompensation

## Status

🟡 **Реализовано (T-F09-067).** `rpc ResolveCompensation` в `fob.orders.v1.OrderFlowService`
(`contracts/proto/fob/orders/v1/order_flow_service.proto`). Размещение и cross-service
доступ — [ADR-040](../../03-architecture/adr/ADR-040-compensation-resolution-cross-service.md);
operator-driven политика — [ADR-039](../../03-architecture/adr/ADR-039-compensation-resolution.md).

## Purpose

Operator (F-16 console) разрешает pending-компенсацию combo, возникшую при провале
внешней ноги (MVP-5, ADR-037). order_flow — операторская поверхность combo и владелец
создания FlowOrder; `combo_compensations` принадлежит matching, поэтому order_flow
читает/фиксирует резолв через `matching.CompensationService` (см.
[matching-compensation-service.md](matching-compensation-service.md)).

## Contract (proto)

```proto
message ResolveCompensationRequest {
  fob.common.v1.EventMeta meta = 1;
  string compensation_id = 2;
  string action          = 3;  // reverse_internal | retry_external | accept
  string operator_id     = 4;  // F-16 operator (audit §22)
}
message ResolveCompensationResponse {
  fob.common.v1.EventMeta meta = 1;
  bool applied                 = 2;  // false → не-pending (идемпотентный no-op)
  repeated string reversing_order_ids = 3;
  fob.common.v1.Error error    = 4;
}
rpc ResolveCompensation(ResolveCompensationRequest) returns (ResolveCompensationResponse);
```

## Actions

| action | Поведение | Статус slice 3b |
| --- | --- | --- |
| `reverse_internal` | по `combo_order_legs.filled_cum` считает реверс (`ComputeReversals`, `cex::common`), создаёт reversing-FlowOrder противоположной стороны через локальный `CreateFlowOrder`, затем `matching.ResolvePending(resolving_ref=order ids)` | ✅ реализовано |
| `accept` | оператор принимает экспозицию; `matching.ResolvePending(accept)` → `cancelled`, без ордеров | ✅ реализовано |
| `retry_external` | re-emit `ExecutionIntent` для внешней ноги | ⛔ `NOT_IMPLEMENTED` (MVP-7) |

## Orchestration (ADR-040 §3)

```
operator → order_flow.ResolveCompensation
  → matching.GetPendingCompensation(compensation_id)      (read; found=false → applied=false)
  → reverse_internal: repo.LoadInternalFilledLegs(parent) (own combo_order_legs)
      → cex::common::ComputeReversals → CreateFlowOrder*   (idem: client_order_id=comp:leg)
  → matching.ResolvePending(action, operator_id, resolving_ref)
```

## Money / invariants / auth

- Деньги идут ТОЛЬКО через `CreateFlowOrder` pipeline — никакого прямого мутирования
  ledger (CLAUDE.md §17). Реверс-FlowOrder ре-валидируется штатным `validate_order`.
- Объём реверса — `combo_order_legs.filled_cum` на момент resolution (ADR-039 §3), не
  устаревший `internal_filled_qty`-снимок.
- Band/rate реверса берутся из исходной ноги (диапазон ценоинвариантен к стороне).
  Market-based pricing и partial-fill математика — deferred MVP-7 (ADR-039).
- Идемпотентность ретрая: `client_order_id = compensation_id:leg_id` (`CreateFlowOrder`
  идемпотентен) + `ResolvePending` gate `status='pending'` (ADR-040 §5). Повтор после
  resolved → `applied=false`.
- operator-auth: как `SetKillSwitch` (dev-skeleton — без крипто-guard; `operator_id` в
  запросе + запись в `combo_compensations.operator_id` = audit, §22). Полноценный guard —
  follow-up (весь dev-стек на InsecureCredentials).
- `instrument_symbol` хранится как `"BASE/QUOTE"` → split на base/quote (нужно для
  ledger-резерва по currency).

## Related

- Feature: `docs/02-system/features/F-09-batch-combo-orders/feature.yaml`
- ADR: ADR-039 (operator-driven), ADR-040 (placement / cross-service)
- Code: `cpp/order_flow/src/app/resolve_compensation_use_case.{hpp,cpp}`,
  `cpp/order_flow/src/infra/matching_compensation_client.{hpp,cpp}`,
  `cpp/order_flow/src/infra/postgres_combo_order_repository.cpp` (`LoadInternalFilledLegs`)
- Tasks: T-F09-067 (this), T-F09-068 (E2E)
