<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F11-01. Подключить новую площадку (operator onboarding)

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-11-external-venues-lob-to-fob](../../features/F-11-external-venues-lob-to-fob/) |
| ☁️ L0 system sequence | [SEQ-UC-F11-01-system](sequences/SEQ-UC-F11-01-system.md) — system как чёрный ящик |
| 🌊 L1 service sequence | [SEQ-F11-UC-F11-01-services](../../../05-components/sequences/SEQ-F11-UC-F11-01-services.md) — взаимодействие сервисов |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-11. External Venues / LOB → FOB](../../features/F-11-external-venues-lob-to-fob/)

## Primary Actor

Operator (через Admin UI или Admin REST API).

## Supporting Actors

- Внешняя площадка (CEX / DEX / AMM) — отвечает на тестовое подключение.
- PostgreSQL `venue_config` — persistent store конфига.
- External Venues Connector — runtime, который выполняет «горячее» подключение.

## Preconditions

- Operator аутентифицирован и имеет роль с правом управления площадками (см. F-16).
- У оператора есть API-credentials или RPC-URL для целевой площадки.
- Сервис `cpp/venues` запущен (`VENUES_ADMIN_HTTP_PORT=8087` слушает).
- PostgreSQL доступен (`VENUES_POSTGRES_DSN` сконфигурирован).

## Trigger

Operator отправляет `POST /api/v1/venues` с конфигом площадки.

## Main Flow

1. Operator готовит payload: `venue_id`, `adapter_mode` (`cex_ws_rest` | `dex_amm_rpc` | `simulator`), `ws_url`/`rest_base_url`/`rpc_url`, `venue_symbol`, `curve_level` (`L1`/`L2`/`L3`), `stale_threshold_ms`, `circuit_breaker_*`, `is_active=false` (для test-then-commit).
2. Operator вызывает `POST /api/v1/venues` (test stage): `is_active=false`.
3. System сохраняет запись в `venue_config` (PostgreSQL) и применяет конфиг в runtime (`VenuesLoop::UpsertVenueConfig`), но без активного polling/subscribe.
4. Operator вызывает `POST /api/v1/venues/{venueId}/reconnect`, чтобы выполнить пробное подключение.
5. System возвращает текущий heartbeat (`status`, `latency_ms`, `health_score`).
6. Если health acceptable, Operator вызывает `POST /api/v1/venues/{venueId}/enable`.
7. System выставляет `is_active=true` в `venue_config` и запускает полноценный poll-loop/subscribe.
8. System публикует первый VenueSnapshot в `venue.snapshots` и первый VenueHealth (RAW) в `venue.health`.
9. Operator видит в Admin UI новую площадку с активным статусом.

## Alternative Flows

### A1. Невалидные параметры конфигурации

1. System (`UpsertVenueConfig`) возвращает `400 Bad Request` с описанием ошибки.
2. PostgreSQL запись не создаётся.

### A2. Площадка недоступна на тестовом подключении

1. Adapter возвращает ошибку через heartbeat (`status=disconnected`, `consecutive_errors>=1`).
2. Operator решает: исправить конфиг (PUT) или удалить площадку (DELETE).

### A3. Hot reload существующей площадки

1. Operator вызывает `PUT /api/v1/venues/{venueId}` с обновлёнными полями.
2. System выполняет `UpsertVenueConfig` без остановки adapter; новые параметры (например, `circuit_breaker_errors`) применяются к следующему циклу.

### A4. Площадка удалена

1. Operator вызывает `DELETE /api/v1/venues/{venueId}`.
2. System выставляет `is_active=false` и останавливает adapter, не удаляя запись (audit trail).

## Postconditions

- Запись в `venue_config` обновлена.
- Если активирована: VenueSnapshot и VenueHealth публикуются.
- Audit-лог `Persisted venue_config via admin API` записан structured-логом.

## Related Sequence Diagrams

- [System sequence (UC-F11-01)](sequences/SEQ-UC-F11-01-system.md)
- [Service sequence (SEQ-F11-01-onboard-venue-services)](../../../05-components/sequences/SEQ-F11-01-onboard-venue-services.md)

## Related Contracts

- [venues.md (REST)](../../../06-api/rest/venues.md)
- [venue-topics.md](../../../06-api/messaging/venue-topics.md)

## Related Components

- [external-venues-connector](../../../05-components/external-venues-connector/overview.md)
- [venue-market-data-normalizer](../../../05-components/venue-market-data-normalizer/overview.md)
- [venue-health-routing](../../../05-components/venue-health-routing/overview.md)

## Related Data

- [venue_config](../../../07-data/venue-config.md) (PostgreSQL)

## Source Fragments

- IN-004 §«Admin API» (F11-19), §«REST API»
