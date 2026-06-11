<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F11-02. Опубликовать VenueSnapshot (raw → normalized)

## Feature

- [F-11. External Venues / LOB → FOB](../../features/F-11-external-venues-lob-to-fob/)

## Primary Actor

System (External Venues Connector — внутренний event-driven цикл).

## Supporting Actors

- Внешняя площадка (CEX / DEX / AMM) — поставщик сырых данных.
- ClickHouse (`venue_snapshots`) — приёмник истории.

## Preconditions

- Площадка активна (`venue_config.is_active=true`).
- Адаптер подключён (WS / REST / RPC).
- Снапшот не помечен `stale` (последнее обновление младше `stale_threshold_ms`).

## Trigger

Внешняя площадка присылает order book update / trade / pool state event.

## Main Flow

1. External Venues Connector получает сырое сообщение (depth update, ticker, pool tick).
2. Connector передаёт raw payload в Venue Market Data Normalizer.
3. Normalizer:
   - канонизирует depth (отсев пылевых уровней, агрегация по `tick_size`/`lot_size`);
   - вычисляет `bestBid`, `bestAsk`, `midPrice`, `spread`;
   - определяет `status` (connected/stale/empty/disconnected);
   - заполняет `fees` (maker/taker), `volume_24h`.
4. Normalizer формирует `fob.venue.v1.VenueSnapshot` с `EventMeta` и публикует в Kafka `venue.snapshots`.
5. SnapshotClickHouseWriter асинхронно пишет запись в ClickHouse `venue_snapshots`.
6. VenueObservabilityProducer публикует RAW heartbeat (`venue.health`, `event_type=RAW`) с latency/error metrics.

## Alternative Flows

### A1. Пустой стакан / dust-only

1. Normalizer выставляет `status="empty"`.
2. VenueSnapshot всё равно публикуется (для аудита и observability), но Curve Builder его игнорирует.

### A2. Snapshot stale

1. Watchdog обнаруживает, что прошло > `stale_threshold_ms` с последнего raw-сообщения.
2. Connector публикует синтетический VenueSnapshot со `status="stale"`.
3. См. [UC-F11-04](../UC-F11-04-venue-health-degradation/use-case.md).

### A3. Адаптер потерял соединение

1. Connector публикует `status="disconnected"` + RAW heartbeat с ошибкой.
2. Запускается reconnect (backoff из `venue_config`).
3. См. [UC-F11-04](../UC-F11-04-venue-health-degradation/use-case.md).

## Postconditions

- В `venue.snapshots` появилось ≥ 1 сообщение.
- ClickHouse содержит соответствующую запись (или fallback warning, если writer недоступен).
- Метрика `venue.snapshots_per_sec` обновлена.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F11-02-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F11-02-publish-snapshot-services.md)

## Related Contracts

- [venue-topics.md → venue.snapshots](../../../06-api/messaging/venue-topics.md#venue-snapshots)
- [`fob.venue.v1.VenueSnapshot`](../../../../contracts/proto/fob/venue/v1/venue.proto)

## Related Components

- [external-venues-connector](../../../05-components/external-venues-connector/overview.md)
- [venue-market-data-normalizer](../../../05-components/venue-market-data-normalizer/overview.md)

## Related Data

- [venue_snapshots](../../../07-data/venue-snapshots.md) (ClickHouse)

## Source Fragments

- IN-004 §«Канонические термины» (VenueSnapshot, Normalizer)
- IN-004 §«Функциональные требования» F11-3, F11-4, F11-16
