# REST API: HedgeFlow Monitor

OpenAPI спецификация: [`contracts/openapi/fob/hedge/v1/api/hedge.yaml`](../../../contracts/openapi/fob/hedge/v1/api/hedge.yaml).

JSON Schema: [`contracts/openapi/fob/hedge/v1/schemas/hedge.json`](../../../contracts/openapi/fob/hedge/v1/schemas/hedge.json).

## Назначение

API для мониторинга и управления хеджированием нетто-позиций на внешних площадках в рамках F-12 Execution Hedge. Доступен оператору (роль `operator` / `admin`) через Admin UI и read-only участникам (роль `provider`).

## Base URL

`/api/v1`

## Authentication

Bearer token (JWT) из Auth & Identity (F-01). RBAC:

- `operator` / `admin` — все методы (включая `POST /hedge/intents/manual`, `POST /hedge/flows/{id}/cancel`, `POST /hedge/flows/{id}/retry`).
- `provider` — read-only методы по своим HedgeFlow.

## Endpoints

### GET `/api/v1/hedge/flows`

Список HedgeFlow с фильтрацией.

**Query parameters:**

| Поле | Тип | Описание |
| --- | --- | --- |
| `provider_id` | string | фильтр по провайдеру |
| `symbol` | string | фильтр по инструменту |
| `status` | enum | `OPEN` / `COMPLETED` / `UNDERFILLED` / `REJECTED` / `RISK_REJECTED` / `CANCELLED` |
| `side` | enum | `BUY` / `SELL` |
| `batch_id` | string | связанный batch (F-04) |
| `date_from`, `date_to` | ISO8601 | временной интервал по `created_at` |
| `page`, `page_size` | int | пагинация (default 1 / 20, max 100) |

**Response 200:** `HedgeFlow[]` (см. `hedge.json#/definitions/HedgeFlow`).

### GET `/api/v1/hedge/flows/{hedgeFlowId}`

Детальная карточка одного HedgeFlow.

**Response 200:** `HedgeFlow` (включая агрегаты `filled_qty`, `avg_fill_price`, `hedge_pnl`, `tot_fee`, статус и timestamps).

**Errors:** 404 (HedgeFlow not found).

### GET `/api/v1/hedge/flows/{hedgeFlowId}/child-orders`

Список child orders, относящихся к HedgeFlow.

**Query parameters:** `status` (enum), `venue_id` (string), `page`, `page_size`.

**Response 200:** `ChildOrder[]`.

### GET `/api/v1/hedge/flows/{hedgeFlowId}/execution-reports`

Timeline ExecutionReport для HedgeFlow.

**Query parameters:** `status` (enum), `page`, `page_size`.

**Response 200:** `ExecutionReport[]` — упорядочено по `timestamp ASC`. Источник: ClickHouse `execution_reports` (см. [07-data/execution-reports.md](../../07-data/execution-reports.md)).

### POST `/api/v1/hedge/intents/manual`

Создание `ExecutionIntent` вручную (operator override). См. [UC-F12-02](../../02-system/use-cases/UC-F12-02-manual-operator-hedge/use-case.md).

**Body:** `ManualIntentRequest`:

| Поле | Тип | Обязательно | Описание |
| --- | --- | --- | --- |
| `provider_id` | string | да | |
| `symbol` | string | да | example: `BTCUSDT` |
| `side` | enum BUY/SELL | да | |
| `target_qty` | number > 0 | да | |
| `urgency` | enum LOW/MEDIUM/HIGH | да | |
| `allowed_venues` | string[] | нет | если не указано — все доступные |
| `price_constraint` | number | нет | max для BUY, min для SELL |
| `max_slippage_bps` | int >= 0 | нет | override policy default |
| `timeout_ms` | int >= 0 | нет | override policy default |

**Response 201:** `HedgeFlow` (status=OPEN).

**Errors:**
- 400 `VALIDATION_ERROR` — параметры невалидны.
- 401 — нет авторизации.
- 403 `PERMISSION_DENIED` — роль не `operator`/`admin`.
- 422 `RISK_REJECTED` — Risk Manager PreHedgeCheck=REJECT.
- 422 `VENUE_UNAVAILABLE` — все запрошенные venues недоступны.

### POST `/api/v1/hedge/flows/{hedgeFlowId}/cancel`

Отмена активного HedgeFlow.

**Response 200:** `HedgeFlow` (обновлённое состояние, `status=CANCELLED`).

**Errors:**
- 403 `PERMISSION_DENIED`.
- 404.
- 409 `INVALID_STATE` — HedgeFlow в terminal state (COMPLETED / REJECTED / UNDERFILLED / CANCELLED).

### POST `/api/v1/hedge/flows/{hedgeFlowId}/retry`

Повторить хедж остатка (для UNDERFILLED / CANCELLED).

**Body (optional):** `RetryRequest`:

| Поле | Тип | Описание |
| --- | --- | --- |
| `urgency_override` | enum LOW/MEDIUM/HIGH | переопределить urgency |
| `allowed_venues` | string[] | ограничить venues |

**Response 201:** `HedgeFlow` (новый HedgeFlow для остатка).

**Errors:**
- 400 `VALIDATION_ERROR`.
- 403 `PERMISSION_DENIED`.
- 404.
- 409 `INVALID_STATE` — текущий статус не допускает retry (например, OPEN).
- 422 `RISK_REJECTED` / `VENUE_UNAVAILABLE` / `UNDERFILLED` / `TIMEOUT`.

## Error Response Schema

```json
{
  "error_code": "string (RISK_REJECTED / VENUE_UNAVAILABLE / ...)",
  "message": "human-readable",
  "details": { "...": "..." }
}
```

## Implementation Status

| Endpoint | Реализация |
| --- | --- |
| GET `/hedge/flows` | ❌ нет backend |
| GET `/hedge/flows/{id}` | ❌ |
| GET `/hedge/flows/{id}/child-orders` | ❌ |
| GET `/hedge/flows/{id}/execution-reports` | ❌ |
| POST `/hedge/intents/manual` | ❌ требует Gateway extension |
| POST `/hedge/flows/{id}/cancel` | ❌ |
| POST `/hedge/flows/{id}/retry` | ❌ |

Все endpoints определены в OpenAPI, реализация в gateway отсутствует.

## Used In Features

- [F-12. Execution Hedge](../../02-system/features/F-12-execution-hedge/)
- [F-16. Operator Console](../../02-system/features/F-16-operator-panel-and-kill-switch/) — Manual Override UI и Cancel/Retry actions.
- [F-17. Monitoring](../../02-system/features/F-17-monitoring-and-alerts/) — Reconciliation Alerts dashboard.

## Used In Use Cases

- [UC-F12-01 — auto-hedge](../../02-system/use-cases/UC-F12-01-auto-hedge-after-batch/use-case.md) (read-only мониторинг)
- [UC-F12-02 — manual hedge](../../02-system/use-cases/UC-F12-02-manual-operator-hedge/use-case.md) (POST `/hedge/intents/manual`)
- [UC-F12-03 — partial fill retry](../../02-system/use-cases/UC-F12-03-partial-fill-retry/use-case.md) (POST `/hedge/flows/{id}/retry`)
- [UC-F12-05 — underfilled](../../02-system/use-cases/UC-F12-05-timeout-underfilled-reconciliation/use-case.md) (cancel/retry)

## Related Components

- [gateway](../../05-components/gateway/overview.md) — HTTP edge.
- [venue-execution-adapter](../../05-components/venue-execution-adapter/overview.md) — writer / source-of-state.
- [execution-planning](../../05-components/execution-planning/overview.md) — обрабатывает manual intent.

## Source Fragments

- IN-005 §9 «REST API»
- `contracts/openapi/fob/hedge/v1/api/hedge.yaml`
