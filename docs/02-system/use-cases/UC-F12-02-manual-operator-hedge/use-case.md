<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F12-02. Manual Operator Hedge

## Feature

- [F-12. Execution Hedge](../../features/F-12-execution-hedge/)

## Primary Actor

Operator (роль `operator` или `admin`).

## Supporting Actors

- Gateway (REST endpoint)
- Execution Planning
- Risk Manager
- Venue Execution Adapter
- External Venues Connector
- External Venue

## Preconditions

- Оператор аутентифицирован и имеет роль `operator` / `admin`.
- Активны venues с `venue.health=CONNECTED`.
- Доступен Admin UI / API.

## Trigger

Оператор вручную решает хеджировать определённый объём (например, при отказе auto-hedge, после kill-switch reset, или для inventory rebalancing). Отправляет POST `/api/v1/hedge/intents/manual` через Admin UI.

## Main Flow

1. Operator открывает Admin UI → Manual Override форму.
2. Заполняет параметры: `provider_id`, `symbol`, `side`, `target_qty`, `urgency`, опционально `allowed_venues`, `price_constraint`, `max_slippage_bps`, `timeout_ms`.
3. UI отправляет `POST /api/v1/hedge/intents/manual` на Gateway.
4. Gateway валидирует токен и роль (`operator` или `admin`).
5. Gateway формирует `ExecutionIntent` с `source = HEDGE_SOURCE_MANUAL_OVERRIDE` и публикует в Kafka `execution.intents`.
6. Execution Planning читает intent, выполняет routing plan (как в UC-F12-01).
7. Risk Manager `PreHedgeCheck`:
   - OK → дальше.
   - REJECT → Gateway возвращает HTTP 422 `RISK_REJECTED` с деталями нарушения.
8. Venue Execution Adapter создаёт HedgeFlow (status=OPEN), child_orders, исполняет на venues.
9. Adapter возвращает Gateway первоначальный HedgeFlow object (HTTP 201).
10. Дальше — как UC-F12-01: ExecutionReport, Ledger, ClickHouse, UI обновление.

## Alternative Flows

### A1. Все venues unavailable

`PreHedgeCheck` или Execution Planning обнаруживает, что все запрошенные venues имеют `venue.health != CONNECTED`.

- Gateway возвращает HTTP 422 `VENUE_UNAVAILABLE` с details `{venues: [...], reason: "CIRCUIT_BREAKER_OPEN"}`.

### A2. Permission denied

Token валиден, но роль не `operator` / `admin`.

- Gateway возвращает HTTP 403 `PERMISSION_DENIED`.

### A3. Validation error

`target_qty <= 0`, неподдерживаемый `symbol`, неверный enum value.

- Gateway возвращает HTTP 400 `VALIDATION_ERROR`.

## Postconditions

- Создан HedgeFlow с `source=MANUAL_OVERRIDE`.
- При успехе — статус трекается в HedgeFlow Monitor UI.
- Audit trail: запись в operator_actions log с user_id, timestamp, intent_id.

## Acceptance Criteria

Покрывает F12-1 (с manual source), F12-2, F12-8, F12-11. Дополнительно: RBAC для operator endpoint.

## Related Sequence Diagrams

- [System sequence: SEQ-UC-F12-02-system](sequences/SEQ-UC-F12-02-system.md)
- Service-level: same path как UC-F12-01 после Gateway → [SEQ-F12-01-auto-hedge-services](../../../05-components/sequences/SEQ-F12-01-auto-hedge-services.md).

## Related Contracts

- REST: [POST /api/v1/hedge/intents/manual](../../../06-api/rest/hedgeflows.md#post-apiv1hedgeintentsmanual)
- Kafka: [execution.intents](../../../06-api/messaging/execution-intents.md)
- OpenAPI: [`contracts/openapi/fob/hedge/v1/api/hedge.yaml`](../../../../contracts/openapi/fob/hedge/v1/api/hedge.yaml)

## Related Components

- [gateway](../../../05-components/gateway/overview.md)
- [execution-planning](../../../05-components/execution-planning/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [venue-execution-adapter](../../../05-components/venue-execution-adapter/overview.md)
- [external-venues](../../../05-components/external-venues/overview.md)

## Related Data

- PostgreSQL: [hedgeflows](../../../07-data/hedgeflows.md) (запись с `source=MANUAL_OVERRIDE`).

## Source Fragments

- IN-005 §9 «REST API» → POST `/api/v1/hedge/intents/manual`
- IN-005 §1 «HedgeSource enum» → MANUAL_OVERRIDE
- `contracts/openapi/fob/hedge/v1/api/hedge.yaml` → operationId `createManualIntent`
