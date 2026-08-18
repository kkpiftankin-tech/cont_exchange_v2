# Data — F-20 sim OLTP (PostgreSQL)

Источник: IN-010. Feature: [F-20](../02-system/features/F-20-live-venue-simulator/README.md).
Owner: **venues** (SimSession Manager). Это **документация целевой схемы**; DDL в
`infra/postgres/init.sql` добавляется на этапе реализации (не в этой docs-итерации).
Деньги/количества — `NUMERIC(38,18)` (mirrors Decimal §9).

## `sim_sessions`

Конфигурационная сессия симуляции: набор моделей + область применения (venues/instruments).

| Колонка | Тип | Назначение |
|---|---|---|
| `sim_session_id` | UUID PK | идентификатор сессии |
| `name` | VARCHAR(255) | человекочитаемое имя |
| `routing_mode` | VARCHAR(20) CHECK(SIM_ONLY/LIVE_ONLY/SHADOW) | режим маршрутизации |
| `scope_venues` | TEXT[] | venues в sim-режиме |
| `scope_instruments` | TEXT[] | инструменты в sim-режиме |
| `latency_model` / `impact_model` / `fee_model` / `rejection_model` | JSONB | конфигурации моделей |
| `stale_lob_threshold_ms` | INTEGER (dflt 2000) | порог свежести LOB |
| `partial_fill_mode` | VARCHAR(20) (dflt LEVEL_BY_LEVEL) | стратегия частичного исполнения |
| `status` | VARCHAR(20) CHECK(ACTIVE/PAUSED/COMPLETED/CANCELLED) | статус |
| `created_at`/`activated_at`/`completed_at`/`created_by` | TIMESTAMPTZ / VARCHAR | аудит |

- **Writer:** SimSession Manager (Admin API CRUD).
- **Reader:** VenueSimRouter, VenueSimulator (через `sim.config`).

## `sim_child_orders`

Журнал симулированных child-ордеров (связка request → sim result), operational.

| Колонка | Тип | Назначение |
|---|---|---|
| `child_order_id` | UUID PK | id child-ордера |
| `sim_session_id` | UUID FK→sim_sessions | сессия |
| `hedge_flow_id`, `client_order_id` | UUID / VARCHAR | трассировка к F-12 |
| `venue_id`, `symbol`, `side`, `order_type` | строки/enum | параметры ордера |
| `qty`, `price` | NUMERIC(38,18) | объём/цена |
| `routing_mode`, `status`, `reject_reason` | строки | режим/исход |
| `execution_id` | UUID | ссылка на итоговый (Sim)ExecutionReport |
| `created_at` | TIMESTAMPTZ | время |

- **Writer:** VenueSimRouter / VenueSimulator. Индексы по `sim_session_id`, `client_order_id`.

## Links

- Feature [F-20](../02-system/features/F-20-live-venue-simulator/README.md) · UC [F20-01](../02-system/use-cases/UC-F20-01-sim-only-execution/use-case.md)
- REST: [sim-sessions-admin-api](../06-api/rest/sim-sessions-admin-api.md) · OLAP: [sim-execution-reports](sim-execution-reports.md)
