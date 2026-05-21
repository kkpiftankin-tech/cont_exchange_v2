---
id: ADR-014
status: Accepted
date: 2026-05-20
owners:
  - core-team
related:
  - docs/02-system/features/F-11-external-venues-lob-to-fob/
  - docs/05-components/external-venues-connector/
  - docs/05-components/venue-market-data-normalizer/
  - docs/05-components/venue-liquidity-curve-builder/
  - docs/05-components/venue-execution-adapter/
  - cpp/venues/
---

# ADR-014: `cpp/venues` как единый бинарь vs отдельные сервисы

> **Note:** Этот ADR изначально был задрафтован как ADR-009 при ingest IN-004 (F-11);
> перенумерован в ADR-014, чтобы избежать коллизии с ADR-009 (Shadow-mode isolation
> for F-15 Replay), задрафтованным параллельным ingest IN-006.

## Контекст

Спецификация IN-004 (F-11. Подключение внешних площадок) описывает пять «компонентов» как самостоятельные сущности:

- External Venues Connector
- Venue Market Data Normalizer
- Venue Liquidity Curve Builder
- Venue Health & Routing Service
- Venue Execution Adapter

В коде после импорта из `origin/dev`:

- `cpp/venues` — один бинарь, который объединяет Connector + Normalizer + Liquidity Curve Builder + Execution Adapter в один процесс (`VenuesLoop`).
- `cpp/venue_health` — отдельный compose-сервис, реализующий Venue Health & Routing.

Поскольку docs-as-code требует прослеживаемости от документации к коду, нужно решить:

1. как соотносить документные «компоненты» (5 штук) и runtime-сервисы (2 штуки);
2. оправдан ли текущий split или его нужно изменить.

## Решение

**Принять текущий split в коде** (`cpp/venues` + `cpp/venue_health`) как канон для MVP. Документные компоненты разнести следующим образом:

| Документный компонент              | Runtime сервис      | Файл `component.yaml`                                                                          |
| ---------------------------------- | ------------------- | ---------------------------------------------------------------------------------------------- |
| external-venues-connector          | `cpp/venues`        | [docs/05-components/external-venues-connector/component.yaml](../../05-components/external-venues-connector/component.yaml) |
| venue-market-data-normalizer       | `cpp/venues`        | [docs/05-components/venue-market-data-normalizer/component.yaml](../../05-components/venue-market-data-normalizer/component.yaml) |
| venue-liquidity-curve-builder      | `cpp/venues`        | [docs/05-components/venue-liquidity-curve-builder/component.yaml](../../05-components/venue-liquidity-curve-builder/component.yaml) |
| venue-execution-adapter            | `cpp/venues`        | [docs/05-components/venue-execution-adapter/component.yaml](../../05-components/venue-execution-adapter/component.yaml) |
| venue-health-routing               | `cpp/venue_health`  | [docs/05-components/venue-health-routing/component.yaml](../../05-components/venue-health-routing/component.yaml) |

Каждый component.yaml содержит поле `runtime:` явно указывающее на compose-сервис. ADR оправдывает группировку:

1. **Connector + Normalizer + Curve Builder** живут в одном процессе, потому что обмениваются in-memory (передача raw → нормализованный → кривая выполняется без сериализации в Kafka между шагами). Промежуточные топики добавили бы Kafka round-trip ~5–20 ms per snapshot, что неприемлемо для SLA p95 ≤ 50 ms на LOB→FOB.
2. **Execution Adapter** живёт в том же бинаре, чтобы переиспользовать live `VenueAdapter` instances (один и тот же WS-соединение используется и для market data, и для исполнения).
3. **Health & Routing** вынесен отдельно, потому что aggregated FSM (circuit breaker) — это per-instance singleton, который должен работать корректно даже при рестарте Connector'а; делить state с Connector'ом нельзя без persistence.

## Альтернативы

### A1. Полностью monolithic (один бинарь, включая Health & Routing)

**Отклонено.** Circuit breaker должен выживать рестарт Connector'а, чтобы не «забывать» о trips. Также — Health Service потребляет venue.health, что естественно даёт separate concerns.

### A2. Five separate services (как декларирует IN-004)

**Отклонено для MVP.** Накладные расходы:

- 5 compose-сервисов вместо 2 — 2.5× больше container overhead;
- 4 дополнительных Kafka топика для intra-feature потока (raw → normalized → curve → synthetic), каждый round-trip > 5 ms;
- Сложнее версионировать совместный protobuf evolution.

При росте нагрузки и потребности в independent scaling — обратимо через T-F11-330. Конкретный split вынесет Curve Builder (CPU-heavy) или Connector (network-heavy) в отдельный сервис.

### A3. Сегодня monolithic, но с явной shared library

**Принято как часть A.** В `cpp/venues` слои `domain` (Normalizer, Curve Builder math) и `infra` (Connector adapters) разделены, что облегчает будущий split.

## Последствия

### Положительные

- Минимальный intra-feature latency (in-memory обмен).
- Меньше операционной сложности (2 контейнера вместо 5).
- Единый CMake target, единые тесты.

### Отрицательные

- Один бинарь = единая точка отказа Connector + Normalizer + Curve Builder. При панике одного из слоёв падает всё.
- Сложнее независимо профилировать CPU между слоями.

### Risk Mitigations

- Изоляция через thread boundaries (`venues_loop` имеет отдельные t_md_ / t_exec_).
- Тесты per-домен (depth_curve_builder_test, normalize_snapshot_test) обеспечивают тестируемость без поднятия всего бинаря.
- Possible future split документирован в T-F11-330.

## Обратимость

**High.** Split возможен через:

1. Создание `cpp/venue_normalizer/` (новый CMake target) — переноса домена normalize + status.
2. Конвертация in-memory передачи в Kafka publish/subscribe для `venue.snapshots`.
3. Adapter remains в cpp/venues; Curve Builder читает `venue.snapshots` через consumer.

Оценка переноса — 5–10 рабочих дней. Сделать имеет смысл только если профайлер покажет CPU-bound в одном из слоёв, который мешает scale-out.

## Связанные документы

- [F-11 README → Conflict Notes C-1](../../02-system/features/F-11-external-venues-lob-to-fob/README.md#conflict-notes)
- [T-F11-330. Decide on Curve Builder split](../../implementation-plan/F-11-external-venues.tasks.md)
- ADR-001 (event-driven microservices) — общий принцип, который этот ADR уточняет для F-11.
