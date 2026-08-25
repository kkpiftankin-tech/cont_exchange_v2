# gRPC Service: matching CompensationService

## Status

🟡 **Контракт материализован (T-F09-064).** `service CompensationService` в
`fob.matching.v1` (`contracts/proto/fob/matching/v1/compensation.proto`). Серверная
реализация — T-F09-065 (matching), client — T-F09-067 (order_flow). Решение о
размещении — [ADR-040](../../03-architecture/adr/ADR-040-compensation-resolution-cross-service.md),
operator-driven политика — [ADR-039](../../03-architecture/adr/ADR-039-compensation-resolution.md).

## Purpose

matching — единственный владелец таблицы `combo_compensations` (CLAUDE.md §14/§17):
в MVP-5 он записывает `pending`-компенсации при провале внешней combo-ноги (ADR-037).
Для MVP-6 operator-driven resolution endpoint живёт в **order_flow** (он владеет
созданием reversing-FlowOrder, §10.3), поэтому matching экспонирует read+resolve
через этот gRPC-сервис. Создание ордеров остаётся вне сервиса — резолв лишь
переводит состояние компенсации и пишет audit.

## Methods

| rpc | Назначение | Идемпотентность |
| --- | --- | --- |
| `ListPendingCompensations` | список pending (опц. фильтр по `parent_order_id`) — operator-console F-16 | read |
| `GetPendingCompensation` | деталь по `compensation_id` — order_flow читает перед `reverse_internal` | read |
| `ResolvePending` | переход `pending → resolved\|cancelled` + audit | да, gate `status='pending'`; повтор → `applied=false`, не ошибка |

## Contract (proto)

```proto
message PendingCompensation {
  string compensation_id   = 1;
  string parent_order_id   = 2;
  string leg_id            = 3;
  string reason            = 4;  // rejected | timeout | cancelled
  fob.common.v1.Decimal internal_filled_qty = 5;
}

message ListPendingCompensationsRequest  { fob.common.v1.EventMeta meta = 1; string parent_order_id = 2; }
message ListPendingCompensationsResponse { fob.common.v1.EventMeta meta = 1; repeated PendingCompensation compensations = 2; }
message GetPendingCompensationRequest    { fob.common.v1.EventMeta meta = 1; string compensation_id = 2; }
message GetPendingCompensationResponse   { fob.common.v1.EventMeta meta = 1; PendingCompensation compensation = 2; bool found = 3; }
message ResolvePendingRequest  { fob.common.v1.EventMeta meta = 1; string compensation_id = 2; string action = 3; string operator_id = 4; string resolving_ref = 5; }
message ResolvePendingResponse { fob.common.v1.EventMeta meta = 1; bool applied = 2; fob.common.v1.Error error = 3; }

service CompensationService {
  rpc ListPendingCompensations(ListPendingCompensationsRequest) returns (ListPendingCompensationsResponse);
  rpc GetPendingCompensation(GetPendingCompensationRequest) returns (GetPendingCompensationResponse);
  rpc ResolvePending(ResolvePendingRequest) returns (ResolvePendingResponse);
}
```

## Money / invariants

- `internal_filled_qty` — `fob.common.v1.Decimal` (никаких double, CLAUDE.md §9). Это
  лишь снимок; объём для `reverse_internal` пересчитывается из
  `combo_order_legs.filled_cum` на момент resolution (ADR-039 §3, ADR-040 §3).
- `action ∈ {reverse_internal, retry_external, accept}` — валидируется сервером;
  неизвестное значение → `error` (не `applied=false`).
- `ResolvePending` идемпотентен: повторный вызов на уже resolved/cancelled →
  `applied=false`, что делает order_flow-оркестрацию безопасной к ретраям (ADR-040 §5).

## Orchestration (ADR-040 §3)

```
operator → order_flow.ResolveCompensation
  → matching.GetPendingCompensation        (read)
  → ComputeReversals (cpp/common) + CreateFlowOrder (order_flow, local)
  → matching.ResolvePending(resolving_ref = reversing_order_ids)   (this service)
```

## Backward compatibility

Новый сервис + новый proto-файл, существующие контракты (`Solver`, `ExecutionGroup`)
не затронуты. Аддитивно, без breaking changes (ADR-033 политика тегов).

## Related

- Feature: `docs/02-system/features/F-09-batch-combo-orders/feature.yaml`
- ADR: ADR-039 (operator-driven resolution), ADR-040 (placement / cross-service)
- Repo: `cpp/matching/src/infra/postgres_combo_compensation_repository.{hpp,cpp}`
- Data: `infra/postgres/init.sql` (`combo_compensations`), `docs/07-data/oltp-schema.md`
- Tasks: T-F09-065 (server), T-F09-067 (order_flow client + endpoint)
