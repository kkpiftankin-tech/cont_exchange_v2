# SEQ-UC-F11-02-system. Publish VenueSnapshot: system view

## Type

System Context Sequence

## Feature

- [F-11](../../../features/F-11-external-venues-lob-to-fob/)

## Use Case

- [UC-F11-02](../use-case.md)

## Purpose

Чёрный ящик ingest'а данных от внешней площадки: venue шлёт raw → Continuous Exchange System нормализует и публикует `VenueSnapshot`. Trader/Matching видят это как обновление market data.

## Participants

- Внешняя площадка (CEX / DEX / AMM)
- Continuous Exchange System
- Trader (косвенно — через UI/market data)

## Diagram

```mermaid
sequenceDiagram
    participant V as External Venue
    participant S as Continuous Exchange System
    actor T as Trader

    V-->>S: depth update / trade / pool event
    S->>S: normalize → VenueSnapshot
    S->>S: persist (Kafka venue.snapshots + ClickHouse)
    S-->>T: обновлённый market data (UI)

    alt stale > threshold
        S->>S: emit VenueSnapshot status=stale
        S-->>T: индикатор «stale» в UI
    end
```

## Related Service Sequence

- [SEQ-F11-02-publish-snapshot-services](../../../../05-components/sequences/SEQ-F11-02-publish-snapshot-services.md)
