# SEQ-CURVE-001. LOB → FOB cycle (внутренний)

## Type

Internal Component Sequence (venue-liquidity-curve-builder)

## Feature

- [F-11](../../../02-system/features/F-11-external-venues-lob-to-fob/)

## Use Case

- [UC-F11-03](../../../02-system/use-cases/UC-F11-03-build-liquidity-curve/use-case.md)

## Purpose

Внутренний цикл построения VenueLiquidityCurve из одной VenueSnapshot: per-side $p(q)$ → $S(q)$ → $L(v)$ + dual, опционально SyntheticFlowOrder.

## Participants

- LiquidityCurveProducer (app)
- DepthCurveBuilder (domain)
- AmmDirectFob (domain, для DEX/AMM)
- L2 Regularizer
- L3 Calibrator (history of execution reports)
- KafkaProducer (venue.liquidity.fob, venue.synthetic)
- PostgresSyntheticOrderRepository

## Diagram

```mermaid
sequenceDiagram
    participant LCP as LiquidityCurveProducer
    participant DCB as DepthCurveBuilder
    participant AMM as AmmDirectFob
    participant REG as L2 Regularizer
    participant CAL as L3 Calibrator
    participant K as Kafka
    participant PG as PostgreSQL synthetic_orders

    Note over LCP: VenueSnapshot in
    alt venue_type == cex
        LCP->>DCB: build_side(bids), build_side(asks)
        DCB-->>LCP: p(q), S(q), L(v)
    else venue_type == dex/amm
        LCP->>AMM: build_from_pool(state)
        AMM-->>LCP: p(q), S(q), L(v)
    end

    alt level == L1
        LCP->>LCP: monotone L(v)
    else level == L2
        LCP->>REG: regularize (Moreau/Tikhonov)
        REG-->>LCP: convex, eps1, eps2
    else level == L3
        LCP->>REG: regularize
        REG-->>LCP: convex, eps1, eps2
        LCP->>CAL: calibrate against execution history
        CAL-->>LCP: eps3, S_L3 = (1-w)*S_model + w*S_exec
    end

    LCP->>LCP: assemble VenueLiquidityCurve + schema/producer version
    LCP->>K: produce venue.liquidity.fob

    opt synthetic_enabled
        LCP->>LCP: derive SyntheticFlowOrder (p_l, p_h, q_rate, q_max)
        LCP->>K: produce venue.synthetic
        LCP->>PG: INSERT INTO synthetic_orders (status=active)
    end
```

## Related

- Service sequence: [SEQ-F11-03-build-curve-services](../../sequences/SEQ-F11-03-build-curve-services.md)
