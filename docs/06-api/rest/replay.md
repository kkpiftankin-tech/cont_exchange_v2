---
id: DOC-API-REST-REPLAY
phase: 06-api
status: implemented-mvp
owner: core-team
source:
  - IN-006 § JSON-схемы + REST API
related:
  - docs/02-system/features/F-15-backtest-replay/
  - contracts/openapi/fob/replay/v1/api/replay.yaml
---

# REST API — Replay (F-15)

Полная спецификация — в [contracts/openapi/fob/replay/v1/api/replay.yaml](../../../contracts/openapi/fob/replay/v1/api/replay.yaml) и [contracts/openapi/fob/replay/v1/schemas/replay.json](../../../contracts/openapi/fob/replay/v1/schemas/replay.json).

Базовый префикс: `/api/v1/replay`.
Сервис: `backtest-service` (port 8087 в dev, через gateway в prod).

## Аутентификация и RBAC

Все endpoints требуют JWT в `Authorization: Bearer <token>`. После валидации
JWT извлекается `UserContext` (см. [audit_types.hpp](../../../cpp/backtest/src/app/audit_types.hpp))
со списком permissions. Permission-mapping:

| Endpoint                                                  | Required permissions                                               | Ownership                       |
| --------------------------------------------------------- | ------------------------------------------------------------------ | ------------------------------- |
| `POST /api/v1/replay/sessions`                            | `replay:create`                                                    | n/a                             |
| `GET /api/v1/replay/sessions`                             | `replay:read`                                                      | `analyst`/`viewer` — только свои |
| `GET /api/v1/replay/sessions/{id}`                        | `replay:read`                                                      | `analyst`/`viewer` — только свои |
| `GET /api/v1/replay/sessions/{id}/summary`                | `replay:read`                                                      | `analyst`/`viewer` — только свои |
| `GET /api/v1/replay/sessions/{id}/agentlogs`              | `replay:read`                                                      | `analyst`/`viewer` — только свои |
| `POST /api/v1/replay/sessions/{id}/retry`                 | `replay:create`                                                    | `analyst` — только свои          |
| `DELETE /api/v1/replay/sessions/{id}`                     | `replay:cancel`                                                    | `analyst` — только свои          |
| `GET /api/v1/replay/compare`                              | `replay:read` (для обеих сессий)                                   | `analyst` — только если обе свои |
| `POST /api/v1/replay/audit-runs`                          | `replay:execute` + role `admin`                                    | admin only                      |

Каждый запрос/ответ пишется в `audit_log` (PostgreSQL).

## POST /api/v1/replay/sessions

Создаёт ReplaySession в статусе `pending`.

### Поле `persist` (F15-PERSIST)

Опциональное булево поле. Умолчание: `true`.

| Значение | Поведение |
| -------- | --------- |
| `true` (или отсутствует) | Стандартное поведение: сессия пишется в PostgreSQL `replay_sessions`/`replay_summaries`, AgentLog — в ClickHouse `replay_agentlogs`. |
| `false` | Ephemeral-режим: сессия живёт только в памяти процесса + в живом Kafka-потоке `replay.results`. Ничего не записывается в PostgreSQL или ClickHouse. GET /sessions/{id}, /summary, /agentlogs, retry возвращают 404/no-data для ephemeral id. Сессия не возвращается в GET /sessions (listing). |

Frontend по умолчанию отправляет `persist: false` для обычных интерактивных запусков; существующие скрипты/тесты, не передающие `persist`, получают поведение `persist: true` без изменений.

### Request

```json
{
  "name": "BTC Strategy Test Q4",
  "user_id": "uuid",
  "date_range_from": "2025-10-01T00:00:00Z",
  "date_range_to": "2025-12-31T23:59:59Z",
  "strategy": [
    {
      "symbol": "BTCUSDT",
      "side": "buy",
      "pL": 58000,
      "pH": 62000,
      "qrate": 0.5,
      "qmax": 100,
      "executionwindow": 3600
    }
  ],
  "solver_config_id": "cfg-001",
  "risk_limits_id": "rlim-001",
  "fee_model": {
    "makerfeerate": 0.0002,
    "takerfeerate": 0.0005
  },
  "reward_mode": "pnl",
  "random_seed": 42,
  "persist": false
}
```

### Response — 201 Created

```json
{
  "session_id": "rpl-abc-123",
  "status": "pending",
  "total_batches": 7890,
  "created_at": "2026-03-12T05:15:00Z"
}
```

### Errors

| Status | error_code               | Description                                                |
| ------ | ------------------------ | ---------------------------------------------------------- |
| 400    | `validation_error`       | Невалидный strategy (см. F15-22..25)                       |
| 400    | `no_historical_data`     | ClickHouse не содержит batchresults в указанном диапазоне  |
| 401    | `unauthorized`           | JWT невалиден                                              |
| 403    | `forbidden`              | Нет permission `replay:create`                              |
| 409    | `session_id_conflict`    | session_id уже существует                                  |

## GET /api/v1/replay/sessions

Возвращает список сессий с фильтрами.

### Query parameters

| Name        | Type            | Default | Description                                |
| ----------- | --------------- | ------- | ------------------------------------------ |
| `user_id`   | uuid            | —       | Фильтр по владельцу (admin only)           |
| `status`    | enum            | —       | `pending`/`running`/`completed`/`failed`/`cancelled` |
| `date_from` | ISO-8601        | —       | Created from                                |
| `date_to`   | ISO-8601        | —       | Created to                                  |
| `page`      | int             | `1`     | 1-based                                     |
| `page_size` | int (max 100)   | `20`    |                                             |

### Response — 200 OK

Массив `ReplaySession` DTO (см. [replay.proto](../../../contracts/proto/fob/replay/v1/replay.proto)).

## GET /api/v1/replay/sessions/{id}

Полная карточка сессии: lifecycle поля, snapshot, error_details при failed.

## GET /api/v1/replay/sessions/{id}/summary

Возвращает `ReplaySummary`.

### Response — 200 OK

```json
{
  "session_id": "rpl-abc-123",
  "summary_id": "smr-xyz",
  "avg_is": -0.00023,
  "total_pnl": 15420.50,
  "avg_pnl": 1.95,
  "std_pnl": 12.30,
  "sharpe": 0.159,
  "fill_rate": 94.2,
  "avg_vwap": 60125.40,
  "avg_solve_time_ms": 342.5,
  "max_drawdown": 2100.00,
  "total_batches": 7890,
  "processed_batches": 7890,
  "failed_batches": 0,
  "total_fill_events": 31240,
  "partial": false,
  "avgis_rule": "volume_weighted",
  "decision_price_source": "marketdata_mid_with_clearprice_fallback",
  "no_requested_volume": false,
  "metrics": {},
  "created_at": "2026-03-12T05:42:00Z"
}
```

## GET /api/v1/replay/sessions/{id}/agentlogs

Возвращает пошаговые AgentLog с пагинацией и фильтрами.

### Query parameters

| Name             | Type    | Description                                  |
| ---------------- | ------- | -------------------------------------------- |
| `batch_seq_from` | int     | Начало диапазона по batch_seq                |
| `batch_seq_to`   | int     | Конец диапазона                              |
| `pnl_min`        | number  | Фильтр по PnL                                |
| `pnl_max`        | number  |                                              |
| `fillrate_min`   | number  | %                                            |
| `fillrate_max`   | number  | %                                            |
| `solver_error`   | boolean | Только батчи с `solver_error_flag=1`         |
| `page`           | int     | 1-based                                      |
| `page_size`      | int     | max 200, default 50                          |

Источник: ClickHouse `replay_agentlogs` (ReplacingMergeTree, читать через `FINAL`).

## POST /api/v1/replay/sessions/{id}/retry

Создаёт новую сессию-повторение failed/cancelled.

### Request

```json
{
  "new_session_id": "rpl-retry-001",
  "user_id": "uuid",
  "use_same_config": true,
  "override_config": null
}
```

Если `use_same_config=false`, передаётся `override_config` с новыми
`solver_config_id`, `risk_limits_id`, `fee_model`.

### Response — 201 Created

Та же структура, что и POST /sessions, но `retry_parent_id` будет
заполнено (через миграцию 003).

## DELETE /api/v1/replay/sessions/{id}

Отменяет сессию (`pending` → `cancelled`, `running` → finish current batch
→ `cancelled` с partial summary).

### Response — 200 OK

ReplaySession DTO с финальным статусом.

| Status | error_code              | Description                              |
| ------ | ----------------------- | ---------------------------------------- |
| 404    | `not_found`             |                                          |
| 409    | `terminal_state`        | Сессия уже completed/failed/cancelled    |

## GET /api/v1/replay/compare

Сравнение двух сессий.

### Query parameters

| Name       | Type | Required | Description |
| ---------- | ---- | -------- | ----------- |
| `sessionA` | uuid | yes      |             |
| `sessionB` | uuid | yes      |             |

### Response — 200 OK

```json
{
  "sessionA": "rpl-001",
  "sessionB": "rpl-002",
  "diff": {
    "avg_is": {"A": -0.00023, "B": -0.00018, "delta": 0.00005, "better": "B"},
    "total_pnl": {"A": 15420.50, "B": 16800.30, "delta": 1379.80, "better": "B"},
    "sharpe": {"A": 0.159, "B": 0.185, "delta": 0.026, "better": "B"},
    "fill_rate": {"A": 94.2, "B": 91.5, "delta": -2.7, "better": "A"},
    "max_drawdown": {"A": 2100.00, "B": 1850.00, "delta": -250.00, "better": "B"},
    "avg_solve_time_ms": {"A": 342.5, "B": 358.0, "delta": 15.5, "better": "A"}
  }
}
```

Direction-aware `better`:

- меньше лучше: `avg_is`, `max_drawdown`, `avg_solve_time_ms`.
- больше лучше: `total_pnl`, `sharpe`, `fill_rate`.

| Status | error_code               | Description                                    |
| ------ | ------------------------ | ---------------------------------------------- |
| 400    | `incompatible_status`    | Хотя бы одна сессия не completed/partial       |
| 400    | `date_range_mismatch`    | Различные date range                            |

## POST /api/v1/replay/audit-runs

Audit-mode replay одного `batch_id`.

### Request

```json
{
  "batch_id": "bat-2025-03-12T05:42:00Z-xyz",
  "tolerance": {
    "residual_norm": 1e-9,
    "clear_price_rel": 1e-6
  },
  "override_config": null
}
```

### Response — 200 OK

```json
{
  "audit_run_id": "aud-001",
  "status": "completed",
  "equivalent": true,
  "diff": {
    "clear_prices": {},
    "executed_rates": {},
    "residual_norm": {"production": 1e-10, "replay": 1.5e-10, "delta": 5e-11}
  },
  "completed_at": "2026-05-20T11:30:00Z"
}
```

## Conflict Notes (OpenAPI vs Source IN-006)

- **CN-IN006-05** — Источник IN-006 использует поля `daterangefrom`/`daterangeto`
  без подчёркиваний и `feemodel.makerfeerate`. Реальный OpenAPI (и снимок
  proto) использует snake_case: `date_range_from`, `fee_model.maker_fee_rate`.
  Источник истины: OpenAPI + proto (deployed contract).
- **CN-IN006-06** — Источник не упоминает audit endpoint явно как REST путь.
  Текущий код реализует `RunReplayAuditBatchUC`; путь зафиксирован здесь как
  `POST /api/v1/replay/audit-runs` (предложение docs). При расхождении с
  фактическим routing в [main.cpp](../../../cpp/backtest/src/main.cpp) —
  актуализировать.
- **CN-IN006-07** — IN-006 не упоминает RBAC. Реализация добавляет
  обязательную авторизацию и audit_log на каждый endpoint.

## Used In

- Feature [F-15](../../02-system/features/F-15-backtest-replay/)
- Use cases UC-F15-01..UC-F15-06
- Sequence diagrams SEQ-F15-01..SEQ-F15-04

## Source Fragments

- IN-006 § JSON-схемы, REST API, Definition of Done
