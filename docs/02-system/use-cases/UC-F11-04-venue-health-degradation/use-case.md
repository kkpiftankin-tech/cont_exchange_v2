# UC-F11-04. Деградация площадки (stale / circuit breaker)

## Feature

- [F-11. External Venues / LOB → FOB](../../features/F-11-external-venues-lob-to-fob/)

## Primary Actor

System (Venue Health & Routing Service).

## Supporting Actors

- External Venues Connector — поставщик RAW heartbeat.
- Operator — получатель алертов и инициатор ручных действий (reconnect/disable).
- Execution Planning (F-12) — потребитель AGGREGATED `VenueHealth`.

## Preconditions

- Площадка активна (`venue_config.is_active=true`).
- В Kafka `venue.health` идут RAW heartbeat'ы.
- `circuit_breaker_enabled=true`, пороги (`CIRCUIT_BREAKER_ERRORS`, `CIRCUIT_BREAKER_WINDOW_S`, `CIRCUIT_BREAKER_COOLDOWN_S`) сконфигурированы.

## Trigger

Одно из:

- последний snapshot старше `stale_threshold_ms` (stale detection);
- количество ошибок `consecutive_errors` достигло `circuit_breaker_errors` за окно `circuit_breaker_window_s` (CB trip);
- внешняя площадка вернула HTTP 5xx / WS close / RPC timeout.

## Main Flow (Stale)

1. External Venues Connector обнаруживает: `now - last_snapshot_ts > stale_threshold_ms`.
2. Connector публикует RAW `VenueHealth` с `status=STALE` в `venue.health`.
3. Venue Health & Routing Service потребляет, обновляет `VenueState`, пересчитывает `health_score`.
4. Service публикует AGGREGATED `VenueHealth` со `status=STALE` и `routing_recommendation=AVOID`.
5. Curve Builder перестаёт публиковать новые кривые для этой площадки.
6. Execution Planning (F-12) видит `avoid` → переключает routing на другую venue.
7. Observability эскалирует alert в Operator UI.

## Main Flow (Circuit Breaker trip)

1. Connector регистрирует ошибку (timeout / 5xx / WS disconnect).
2. RAW heartbeat: `circuit_breaker_state=CLOSED`, `consecutive_errors++`.
3. При достижении `CIRCUIT_BREAKER_ERRORS` в окне `CIRCUIT_BREAKER_WINDOW_S`:
   - CB переходит в `OPEN`;
   - публикуется RAW + AGGREGATED `VenueHealth` со `status=DISCONNECTED`, `routing_recommendation=BLOCK`.
4. ExecutionIntents для этой площадки отклоняются на стороне adapter (см. UC-F11-05).
5. После `CIRCUIT_BREAKER_COOLDOWN_S` CB переходит в `HALF_OPEN`.
6. Adapter делает пробный запрос; при успехе CB → `CLOSED`, `routing_recommendation=ALLOW`.
7. При неуспехе → `OPEN` снова.

## Alternative Flows

### A1. Manual reconnect от оператора

1. Operator вызывает `POST /api/v1/venues/{venueId}/reconnect` — см. [UC-F11-01](../UC-F11-01-onboard-venue/use-case.md) A3.

### A2. Manual disable от оператора

1. Operator вызывает `POST /api/v1/venues/{venueId}/disable`.
2. System выставляет `is_active=false`, останавливает adapter, публикует финальный AGGREGATED `VenueHealth` со `status=DISCONNECTED`, `reason="operator_disable"`.

### A3. Площадка восстановилась автоматически (без manual reconnect)

1. Adapter получает первый успешный raw-message → `consecutive_errors=0`.
2. Stale / CB сбрасываются.
3. Публикуется AGGREGATED `VenueHealth` со `status=OK`, `routing_recommendation=ALLOW`.

## Postconditions

- AGGREGATED `VenueHealth` отражает актуальный `health_score` и `routing_recommendation`.
- Execution Planning не направляет hedge на площадки с `BLOCK`/`AVOID`.
- Operator-метрики обновлены; alert закрывается при возврате в `ALLOW`.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F11-04-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F11-04-health-routing-services.md)

## Related Contracts

- [venue-topics.md → venue.health](../../../06-api/messaging/venue-topics.md#venue-health)
- [`fob.venue.v1.VenueHealth`](../../../../contracts/proto/fob/venue/v1/venue.proto) (CircuitBreakerState, VenueHealthStatus, RoutingRecommendation)

## Related Components

- [venue-health-routing](../../../05-components/venue-health-routing/overview.md)
- [external-venues-connector](../../../05-components/external-venues-connector/overview.md)

## Source Fragments

- IN-004 §«Алгоритм Circuit Breaker»
- IN-004 §«Функциональные требования» F11-13, F11-16, F11-17
- IN-004 §«Нефункциональные требования» (venue.health ≤ 1 s)
