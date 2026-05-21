# Компонент: venue-health-routing

> **Runtime:** отдельный compose-сервис `cpp/venue_health` (в отличие от Connector/Normalizer/Curve Builder, которые живут в `cpp/venues`).

## Назначение

Aggregator поверх RAW heartbeat'ов. Service:

1. Подписан на Kafka `venue.health` (фильтрует `event_type=RAW` через consumer).
2. Поддерживает per-venue `VenueState` с sliding window метрик (latency, errors, snapshots/sec, stale_ms).
3. Управляет `CircuitBreaker` FSM:
   - `CLOSED` — нормальная работа.
   - `OPEN` — `consecutive_errors ≥ CIRCUIT_BREAKER_ERRORS` за `CIRCUIT_BREAKER_WINDOW_S`; блокировка ExecutionIntents.
   - `HALF_OPEN` — после `CIRCUIT_BREAKER_COOLDOWN_S` пробные запросы.
4. Считает `health_score ∈ [0, 1]` и `routing_recommendation` (`ALLOW`/`CAUTION`/`AVOID`/`BLOCK`).
5. Публикует AGGREGATED `VenueHealth` в тот же топик `venue.health` для downstream (Execution Planning, Risk, observability).

## Алгоритм

Stale detection: если `now - last_snapshot_ts > STALE_THRESHOLD_MS` → `status=STALE`.

Health score (упрощённая формула, см. [main.cpp](../../../cpp/venues/src/main.cpp#L186-L192)):

$$
\text{health\_score} = \mathrm{clamp}_{[0,1]}\Bigl(1 - \pi_{\text{status}} - \pi_{\text{breaker}} - \min(0.25, \mathrm{error\_rate})\Bigr)
$$

где $\pi_{\text{status}}$ — penalty по `VenueConnectionStatus` (connected=0, empty=0.20, stale=0.25, disconnected=0.50), $\pi_{\text{breaker}}$ — penalty по `CircuitBreakerState` (CLOSED=0, HALF_OPEN=0.25, OPEN=0.50).

Routing recommendation — табличная решётка (см. UC-F11-04).

## Конфигурация

| env                          | Default | Назначение                              |
| ---------------------------- | ------- | --------------------------------------- |
| `KAFKA_BROKERS`              | redpanda:9092 | Kafka                            |
| `CIRCUIT_BREAKER_WINDOW_S`   | 60      | Окно для подсчёта ошибок               |
| `CIRCUIT_BREAKER_COOLDOWN_S` | 300     | Длительность OPEN перед HALF_OPEN      |
| `CIRCUIT_BREAKER_ERRORS`     | 3       | Порог trip                              |
| `STALE_THRESHOLD_MS`         | 15000   | Порог stale                             |

## Связанные фичи

- F-11 (primary).
- F-12 — основной потребитель `routing_recommendation`.
- F-08 (Liquidation) — потенциальный потребитель для venue-level kill-switch.
- F-16 (Operator Console) — UI отображения health.

## Participates In Features

- [F-11](../../02-system/features/F-11-external-venues-lob-to-fob/), [F-12](../../02-system/features/F-12-execution-hedge/), [F-08](../../02-system/features/F-08-posttrade-risk-and-liquidations/), [F-16](../../02-system/features/F-16-operator-console/)

## Participates In Use Cases

- [UC-F11-04](../../02-system/use-cases/UC-F11-04-venue-health-degradation/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F11-04-health-routing-services](../sequences/SEQ-F11-04-health-routing-services.md)

## Produced Events

- [venue.health (AGGREGATED)](../../06-api/messaging/venue-topics.md#venue-health)

## Consumed Events

- [venue.health (RAW filter)](../../06-api/messaging/venue-topics.md#venue-health)

## Data Access

- (in-memory only)
