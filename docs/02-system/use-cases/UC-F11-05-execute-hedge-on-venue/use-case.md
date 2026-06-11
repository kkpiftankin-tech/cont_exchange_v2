<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F11-05. Исполнить hedge child-ордер на площадке (F-11 adapter view)

## Feature

- [F-11. External Venues / LOB → FOB](../../features/F-11-external-venues-lob-to-fob/) (adapter уровня)
- [F-12. Execution Hedge](../../features/F-12-execution-hedge/) (бизнес-логика)

> **Scope:** этот UC описывает только adapter-роль `cpp/venues` в исполнении внешнего ордера. Бизнес-логика хеджирования (когда и сколько хеджировать, выбор площадки, allocation) принадлежит F-12 / UC-F12-01 (auto-hedge after batch).

## Primary Actor

System (Venue Execution Adapter — внутри `cpp/venues`).

## Supporting Actors

- Execution Planning (F-12) — поставщик `ExecutionIntent`.
- Внешняя площадка (CEX/DEX/AMM) — исполнитель.
- Ledger — получатель ExecutionReport для применения к балансам.

## Preconditions

- Площадка `venue_config.is_active=true`.
- `circuit_breaker_state=CLOSED` или `HALF_OPEN` (если `OPEN` — intent отклоняется).
- `routing_recommendation ∈ {ALLOW, CAUTION}`.

## Trigger

В Kafka `execution.intents` поступает `fob.execution.v1.ExecutionIntent` с целевой `venue_id`.

## Main Flow

1. Adapter (`ExecutionIntentsConsumer`) принимает `ExecutionIntent`.
2. Adapter проверяет CB-состояние и `routing_recommendation` для целевой площадки.
3. Adapter выбирает конкретный venue-адаптер (`find_adapter(venue_id)`).
4. Adapter формирует child-orders в venue-native формате (учитывая `tick_size`, `lot_size`, fees из последнего VenueSnapshot).
5. Adapter отправляет child-orders на площадку (REST/WS/RPC).
6. Площадка возвращает fill / partial / reject / cancel / timeout.
7. Adapter формирует `fob.execution.v1.ExecutionReport` и публикует в Kafka `execution.venue` (и legacy `execution.reports` для совместимости).
8. Ledger потребляет `execution.venue` и применяет к hedge-балансу (см. F-12).

## Alternative Flows

### A1. CB OPEN

1. Adapter отклоняет intent: публикует `ExecutionReport` со `status=REJECTED`, `reason="circuit_breaker_open"`.

### A2. Partial fill

1. Adapter формирует серию `ExecutionReport`'ов: один на каждый fill chunk.
2. Final report со `status=PARTIAL_FILL` или `FILLED`.

### A3. Timeout

1. Adapter ждёт `request_timeout_ms`; если no response — публикует `status=TIMEOUT`, `reason="venue_timeout"`.
2. Bumps `consecutive_errors` для CB.

### A4. Backtest mode

1. Если включён backtest session (`bt_session` установлен), `ExecutionReport` идёт в `backtest.execution.venue` вместо `execution.venue`.

## Postconditions

- В `execution.venue` опубликован финальный `ExecutionReport`.
- Метрики adapter обновлены (`fills_count`, `rejects_count`, `timeouts_count`).
- При ошибке — `consecutive_errors` для CB.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F11-05-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F11-05-execute-on-venue-services.md)
- F-12 cross-link: UC-F12-01 (auto-hedge), UC-F12-03 (partial fill retry), UC-F12-04 (rejection fallback), UC-F12-05 (timeout/underfill reconciliation)

## Related Contracts

- [`fob.execution.v1.ExecutionIntent`](../../../../contracts/proto/fob/execution/v1/execution.proto)
- [`fob.execution.v1.ExecutionReport`](../../../../contracts/proto/fob/execution/v1/execution.proto)
- [venue-topics.md → execution.venue](../../../06-api/messaging/venue-topics.md#execution-venue)

## Related Components

- [venue-execution-adapter](../../../05-components/venue-execution-adapter/overview.md)
- [external-venues-connector](../../../05-components/external-venues-connector/overview.md)
- [venue-health-routing](../../../05-components/venue-health-routing/overview.md)

## Source Fragments

- IN-004 §«Функциональные требования» F11-15
- IN-004 §«Тестовые кейсы» I3 «Execution hedge: ExecutionIntent → Adapter → EVC → execution.venue → Ledger»
