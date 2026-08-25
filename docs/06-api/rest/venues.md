---
id: DOC-API-REST-VENUES
phase: 06-api
status: draft
owner: core-team
source:
  - IN-004 §«REST API»
related:
  - docs/02-system/features/F-11-external-venues-lob-to-fob/
  - docs/07-data/venue-config.md
  - cpp/venues/src/main.cpp
---

# REST API: External Venues (F-11)

Admin/observability HTTP API сервиса `cpp/venues`. Реализован на Crow (см. [cpp/venues/src/main.cpp](../../../cpp/venues/src/main.cpp)).

- **Base path:** `/api/v1`
- **Default port:** `VENUES_ADMIN_HTTP_PORT=8087`
- **Content-Type:** `application/json`
- **Auth:** TBD (в MVP — отсутствует; для prod — обязательно см. F-16 / F-01).

> **Legacy:** Дополнительно сервис экспонирует `/admin/v1/venue-config/*` — старый namespace для CRUD `venue_config`. Использовать `/api/v1/venues/*` для всех новых клиентов; `/admin/v1/*` считать deprecated.

## Endpoints

| Метод   | Endpoint                                      | Назначение                                           |
| ------- | --------------------------------------------- | ---------------------------------------------------- |
| GET     | `/api/v1/venues`                              | Список всех площадок (config + health + metrics)     |
| GET     | `/api/v1/venues/{venueId}`                    | Детали площадки + последний snapshot + последняя curve |
| POST    | `/api/v1/venues`                              | Добавить или обновить площадку (upsert)              |
| PUT     | `/api/v1/venues/{venueId}`                    | Обновить конфигурацию (full overwrite полей)         |
| DELETE  | `/api/v1/venues/{venueId}`                    | Деактивировать (`is_active=false`)                   |
| POST    | `/api/v1/venues/{venueId}/reconnect`          | Force reconnect adapter                              |
| POST    | `/api/v1/venues/{venueId}/enable`             | `is_active=true`                                     |
| POST    | `/api/v1/venues/{venueId}/disable`            | `is_active=false`                                    |
| POST    | `/api/v1/venues/{venueId}/routing-mode`       | Switch routing mode (`auto` / `watch`)               |
| GET     | `/api/v1/venues/{venueId}/snapshots`          | История VenueSnapshot (limit query param)            |
| GET     | `/api/v1/venues/{venueId}/curves`             | История VenueLiquidityCurve (limit query param)      |
| GET     | `/api/v1/venues/{venueId}/synthetics`         | Текущие SyntheticFlowOrder                           |
| GET     | `/api/v1/venues/health`                       | Сводный health-score по всем площадкам               |
| GET     | `/healthz`                                    | Liveness probe                                       |

## Common types

### VenueConfig (JSON)

```json
{
  "venue_id": "binance",
  "adapter_mode": "cex_ws_rest",
  "ws_url": "wss://stream.binance.com:9443/ws",
  "rest_base_url": "https://api.binance.com",
  "rpc_url": "",
  "chain_id": "",
  "pool_address": "",
  "venue_symbol": "BTCUSDT",
  "depth_levels": 20,
  "curve_level": "L3",
  "synthetic_enabled": false,
  "stale_threshold_ms": 3000,
  "circuit_breaker_enabled": true,
  "circuit_breaker_errors": 10,
  "circuit_breaker_window_ms": 30000,
  "circuit_breaker_cooldown_ms": 30000,
  "is_active": true,
  "routing_mode": "auto",
  "updated_at_ms": 1709283600000
}
```

### VenueHealth (JSON)

```json
{
  "venue_id": "binance",
  "status": "connected",
  "venue_type": "cex",
  "timestamp_ms": 1709283600500,
  "latency_ms": 45,
  "stale_ms": 78,
  "reconnect_attempts": 0,
  "consecutive_errors": 0,
  "connect_success_rate": 1.0,
  "reconnect_success_rate": 1.0,
  "circuit_breaker_state": "CLOSED",
  "circuit_breaker_reason": "",
  "circuit_breaker_error_count": 0,
  "health_score": 0.97,
  "error_rate": 0.0,
  "routing_recommendation": "allow"
}
```

### VenueRuntimeMetrics (JSON)

```json
{
  "stale_rate": 0.0,
  "snapshots_per_sec": 12.5,
  "last_curve_build_latency_ms": 8.2,
  "last_curve_confidence": 0.94,
  "last_curve_requested_level": "L3",
  "last_curve_effective_level": "L2",
  "last_curve_degradation_reason": "no_calibration_history",
  "last_curve_quality_action": "publish_l2_fallback"
}
```

## GET /api/v1/venues

Возвращает список всех площадок с конфигом, health и метриками.

**Response 200:**

```json
{
  "items": [
    {
      "config": { /* VenueConfig */ },
      "health": { /* VenueHealth */ },
      "metrics": { /* VenueRuntimeMetrics */ }
    }
  ],
  "count": 1
}
```

## GET /api/v1/venues/{venueId}

**Response 200:**

```json
{
  "config": { /* VenueConfig */ },
  "health": { /* VenueHealth */ },
  "metrics": { /* VenueRuntimeMetrics */ },
  "last_snapshot": { /* VenueSnapshot JSON */ },
  "latest_curve": { /* VenueLiquidityCurve JSON */ }
}
```

**Response 404:** `venue not found`.

## POST /api/v1/venues

Body — `VenueConfig` (минимально требуется `venue_id`).

**Response 200:** обновлённый `VenueConfig`.
**Response 400:** `venue_id is required` / `failed to upsert venue config`.
**Response 500:** `failed to persist venue config` (если PostgreSQL недоступен).

## PUT /api/v1/venues/{venueId}

То же, что POST, но `venueId` берётся из path.

## DELETE /api/v1/venues/{venueId}

Деактивирует площадку. **Запись не удаляется** из `venue_config` (audit). Если `VENUES_POSTGRES_DSN` сконфигурирован, поле `is_active` сбрасывается в `false`.

**Response 200:** `"deactivated"`.
**Response 404:** `venue not found`.

## POST /api/v1/venues/{venueId}/reconnect

Принудительный reconnect adapter без изменения `is_active`.

**Response 200:** `"reconnect triggered"`.
**Response 400:** ошибка adapter (например, площадка не сконфигурирована).

## POST /api/v1/venues/{venueId}/enable

Кратко: эквивалент `PUT … {"is_active": true}`.

## POST /api/v1/venues/{venueId}/disable

Эквивалент `PUT … {"is_active": false}`.

## POST /api/v1/venues/{venueId}/routing-mode

Body: `{"mode": "auto" | "watch"}`.

- `auto` — routing полностью автоматический (по `routing_recommendation` из venue.health).
- `watch` — площадка только наблюдается, routing на неё не направляется автоматически.

## GET /api/v1/venues/{venueId}/snapshots

Query: `?limit=100` (max 1000).

**Response 200:**

```json
{ "items": [/* VenueSnapshot[] */], "count": 100 }
```

## GET /api/v1/venues/{venueId}/curves

Query: `?limit=100`.

**Response 200:**

```json
{ "items": [/* VenueLiquidityCurve[] */], "count": 100 }
```

## GET /api/v1/venues/{venueId}/synthetics

Query: `?limit=100`.

**Response 200:**

```json
{ "items": [/* SyntheticFlowOrder[] */], "count": 100 }
```

## GET /api/v1/venues/health

Сводный список AGGREGATED health-state по всем площадкам.

**Response 200:**

```json
{
  "items": [
    { /* VenueHealth */ }
  ],
  "count": 3
}
```

## Error codes

| Код  | Условие                                                       |
| ---- | ------------------------------------------------------------- |
| 200  | OK                                                            |
| 400  | Invalid JSON / `venue_id is required` / upsert validation     |
| 404  | `venue config not found` / `venue not found`                  |
| 500  | `failed to persist venue config` (PostgreSQL ошибка)          |

## RBAC (planned)

В MVP API открыт. Целевое разделение ролей (см. F-16):

| Роль      | GET | POST/PUT/DELETE | reconnect/enable/disable |
| --------- | --- | --------------- | ------------------------ |
| viewer    | ✅  | ❌              | ❌                       |
| operator  | ✅  | ✅              | ✅                       |
| admin     | ✅  | ✅              | ✅ + audit log            |

## Used In

- [F-11 README → REST API section](../../02-system/features/F-11-external-venues-lob-to-fob/README.md)
- [UC-F11-01 Onboard venue](../../02-system/use-cases/UC-F11-01-onboard-venue/use-case.md)
- [SEQ-F11-01-onboard-venue-services](../../05-components/sequences/SEQ-F11-01-onboard-venue-services.md)
