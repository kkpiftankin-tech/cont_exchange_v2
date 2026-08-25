# REST: Combo Compensations (F-16 console BFF)

## Status

🟡 **Контракт (T-F09-070-fe / slice 4).** Реализуется в node-BFF `frontend/api/server.js`
(проксирует в gRPC). UI-спека: `docs/frontend/specs/F16-combo-compensation-resolution.md`.
Решения: [ADR-039](../../03-architecture/adr/ADR-039-compensation-resolution.md),
[ADR-040](../../03-architecture/adr/ADR-040-compensation-resolution-cross-service.md).

## Purpose

REST-поверхность для F-16 operator-console разрешения combo-компенсаций. BFF
(`frontend/api/server.js`) — единственный edge для фронта; проксирует в два gRPC:
read → `matching.CompensationService`, resolve → `order_flow.OrderFlowService`
(ADR-040: matching владеет таблицей, order_flow создаёт reversing-ордера).

## Endpoints

### GET `/api/v1/combo-compensations`

Query: `status=pending` (единственный поддерживаемый в MVP-6; иначе — пустой/400).

gRPC backing: `matching.CompensationService/ListPendingCompensations`
(`docs/06-api/grpc/matching-compensation-service.md`).

**200 Response:**
```json
{
  "compensations": [
    {
      "compensationId": "uuid",
      "parentOrderId": "uuid",
      "legId": "uuid",
      "reason": "rejected | timeout | cancelled",
      "internalFilledQty": "decimal-string"
    }
  ],
  "generatedAt": "ISO8601"
}
```
- `internalFilledQty` — строка из proto `Decimal` (`units*10^-scale`), отрендеренная
  BFF в каноническую строку. Никакого float (CLAUDE.md §9).
- Пустой список — нормальное состояние (нет pending).

**5xx** — gRPC недоступен/ошибка: `{ "error": { "code", "message" } }`.

### POST `/api/v1/combo-compensations/{compensationId}/resolve`

gRPC backing: `order_flow.OrderFlowService/ResolveCompensation`
(`docs/06-api/grpc/order-flow-resolve-compensation.md`).

**Request body:**
```json
{ "action": "reverse_internal | retry_external | accept", "operatorId": "non-empty" }
```

**200 Response:**
```json
{ "applied": true, "reversingOrderIds": ["uuid"], "error": null }
```
- `applied:true` — переход pending→resolved|cancelled выполнен; для `reverse_internal`
  созданы reversing-FlowOrder (id в `reversingOrderIds`).
- `applied:false, error:null` — уже не-pending (идемпотентный no-op, ADR-040 §5).
- `applied:false, error:{code,message}` — бизнес-ошибка (`INVALID_ACTION`,
  `NOT_IMPLEMENTED` для retry_external в MVP-6, и т.п.). HTTP 200 (ошибка в payload).

**400** — `operatorId` пуст или `action` отсутствует (валидация BFF до gRPC).

## Money / auth / invariants

- Деньги (reverse) идут через order_flow `CreateFlowOrder` pipeline — BFF их не трогает.
- `operatorId` обязателен и пишется в audit (`combo_compensations.operator_id`, §22).
  operator-auth — как kill-switch (dev-skeleton без крипто-guard).
- Идемпотентность: повтор resolve после resolved → `applied:false` (no-op).
- BFF только маппит proto↔JSON; никакой бизнес-логики.

## BFF env

- `MATCHING_GRPC_ADDR` (default `matching:50053`) — CompensationService.
- `ORDER_FLOW_GRPC_ADDR` (default `order_flow:50051`) — OrderFlowService.

## Related

- UI spec: `docs/frontend/specs/F16-combo-compensation-resolution.md`
- gRPC: matching-compensation-service.md, order-flow-resolve-compensation.md
- Data: `docs/07-data/oltp-schema.md` (`combo_compensations`)
