# SEQ-F11-03-build-curve-services. Build VenueLiquidityCurve: service view

## Type

Service Interaction Sequence

## Feature

- [F-11](../../02-system/features/F-11-external-venues-lob-to-fob/)

## Use Case

- [UC-F11-03](../../02-system/use-cases/UC-F11-03-build-liquidity-curve/use-case.md)

## Purpose

Поток LOB → FOB: SnapshotProducer передаёт VenueSnapshot в LiquidityCurveProducer, который строит p(q)/S(q)/L(v), публикует `VenueLiquidityCurve` в Kafka + опционально материализует `SyntheticFlowOrder` (Kafka + PostgreSQL).

## Participants

- SnapshotProducer (cpp/venues, см. SEQ-F11-02)
- LiquidityCurveProducer (cpp/venues — app)
- DepthCurveBuilder / AmmDirectFob (cpp/venues — domain)
- Kafka (`venue.liquidity.fob`, `venue.synthetic`)
- PostgresSyntheticOrderRepository (cpp/venues — infra)
- PostgreSQL `synthetic_orders`
- ClickHouse `venue_liquidity_curves` (ingestion pending)
- Matching / Execution Planning (consumers, см. F-04 / F-12)

## Diagram

```mermaid
sequenceDiagram
    participant SP as SnapshotProducer
    participant LCP as LiquidityCurveProducer
    participant BUILD as DepthCurveBuilder / AmmDirectFob
    participant K as Kafka
    participant PG as PostgreSQL synthetic_orders
    participant CH as ClickHouse venue_liquidity_curves
    participant M as matching / Execution Planning

    SP->>LCP: publish_snapshot(snapshot, normalized)
    activate LCP
    LCP->>BUILD: build p(q), S(q), L(v) for side
    Note over BUILD: L1: monotone<br/>L2: + Moreau/Tikhonov, eps1/eps2<br/>L3: + calibration, eps3
    BUILD-->>LCP: SideLiquidityCurve {q_grid, p_of_q, s_of_q, v_grid, l_of_v, p_star_grid, s_star_of_p, q_star_of_p}
    LCP->>LCP: assemble VenueLiquidityCurve + schema_version + producer_version

    LCP->>K: produce venue.liquidity.fob (key={venue}|{symbol})
    deactivate LCP

    opt synthetic_enabled
        LCP->>LCP: derive SyntheticFlowOrder (p_l, p_h, q_rate, q_max)
        LCP->>K: produce venue.synthetic
        LCP->>PG: INSERT INTO synthetic_orders (status=active)
    end

    K-->>M: consume venue.liquidity.fob / venue.synthetic
    K-->>CH: (planned) consume venue.liquidity.fob → ClickHouse
```

## Contract Binding Table

| Step                              | Transport | Contract                                                                                | Location                                                                                                                          |
| --------------------------------- | --------- | --------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| LCP → Kafka                       | Kafka     | `venue.liquidity.fob`, `fob.venue.v1.VenueLiquidityCurve`                               | [docs/06-api/messaging/venue-topics.md#venue-liquidity-fob](../../06-api/messaging/venue-topics.md#venue-liquidity-fob)            |
| LCP → Kafka                       | Kafka     | `venue.synthetic`, `fob.orders.v1.SyntheticFlowOrder`                                   | [docs/06-api/messaging/venue-topics.md#venue-synthetic](../../06-api/messaging/venue-topics.md#venue-synthetic)                    |
| LCP → PostgreSQL                  | SQL       | `INSERT INTO synthetic_orders … status='active'`                                        | [docs/07-data/synthetic-orders.md](../../07-data/synthetic-orders.md)                                                              |
| (planned) Kafka → ClickHouse      | Kafka     | `venue.liquidity.fob` → `venue_liquidity_curves`                                        | [docs/07-data/venue-liquidity-curves.md](../../07-data/venue-liquidity-curves.md)                                                  |
| matching ← Kafka                  | Kafka     | `venue.liquidity.fob` consumer-group `matching-external-venues`                          | [cpp/matching/tests/app/external_venue_filter_test.cpp](../../../cpp/matching/tests/app/external_venue_filter_test.cpp) (filter)   |

## Data Binding Table

| Data Object                  | Storage     | Notes                                                                                |
| ---------------------------- | ----------- | ------------------------------------------------------------------------------------ |
| `venue_liquidity_curves`     | ClickHouse  | history с retention ≥ 90 дней                                                        |
| `synthetic_orders`           | PostgreSQL  | lifecycle `active` → `expired` (по TTL) → `used` (после matching)                    |
| last curve cache             | in-memory   | `VenuesLoop::last_curves_` для `GET /api/v1/venues/{id}/curves`                      |

## Related Components

- [venue-liquidity-curve-builder](../venue-liquidity-curve-builder/overview.md)
- [venue-market-data-normalizer](../venue-market-data-normalizer/overview.md)
