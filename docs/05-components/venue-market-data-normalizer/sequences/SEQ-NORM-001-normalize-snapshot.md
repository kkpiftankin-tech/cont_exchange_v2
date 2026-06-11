<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-NORM-001-normalize-snapshot
level: fish
component: venue-market-data-normalizer
---
-->

# SEQ-NORM-001. Normalize raw → VenueSnapshot

## Type

Internal Component Sequence (venue-market-data-normalizer)

## Feature

- [F-11](../../../02-system/features/F-11-external-venues-lob-to-fob/)

## Use Case

- [UC-F11-02](../../../02-system/use-cases/UC-F11-02-publish-snapshot/use-case.md)

## Purpose

Внутренний поток нормализации: сырое сообщение → канонизированный depth → расчёт mid/spread/status → публикация в Kafka + ClickHouse.

## Participants

- VenuesLoop (caller)
- DepthCanonicalizer (domain)
- normalize_snapshot (domain)
- SnapshotStatus (domain)
- SnapshotProducer (app)
- SnapshotClickHouseWriter (infra)
- KafkaProducer venue.snapshots

## Diagram

```mermaid
sequenceDiagram
    participant LOOP as VenuesLoop
    participant DC as DepthCanonicalizer
    participant NORM as normalize_snapshot
    participant ST as SnapshotStatus
    participant SP as SnapshotProducer
    participant CHW as SnapshotClickHouseWriter
    participant K as Kafka venue.snapshots

    LOOP->>DC: canonicalize(raw_bids, raw_asks, tick, lot)
    DC-->>LOOP: cleaned levels
    LOOP->>NORM: build VenueSnapshot
    NORM->>NORM: compute bestBid, bestAsk, mid, spread
    LOOP->>ST: classify_status(last_update_ts, threshold)
    ST-->>LOOP: connected | stale | empty | disconnected
    NORM-->>SP: VenueSnapshot
    SP->>K: produce venue.snapshots
    SP->>CHW: append (async)
```

## Related

- Service sequence: [SEQ-F11-02-publish-snapshot-services](../../sequences/SEQ-F11-02-publish-snapshot-services.md)
