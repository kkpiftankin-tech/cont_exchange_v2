# SEQ-F20-UC-F20-01-services — SIM_ONLY execution (service-level)

- Feature: [F-20](../../02-system/features/F-20-live-venue-simulator/README.md)
- Use case: [UC-F20-01](../../02-system/use-cases/UC-F20-01-sim-only-execution/use-case.md)
- Level: L1 (cross-component)
- Components: VenueSimRouter, VenueSimulator (F-20), Venue Execution Adapter (F-12), EVC + Normalizer (F-11), Settlement Ledger, ClickHouse.

## Contract binding

| Стрелка | Transport | Contract | Doc |
|---|---|---|---|
| Normalizer → `venue.snapshots` → Simulator | Kafka | VenueSnapshot | [messaging/topics](../../06-api/messaging/topics.md) (F-11) |
| VEA → Router | in-process/gRPC | ChildOrderRequest | [messaging/sim-topics](../../06-api/messaging/sim-topics.md) |
| Simulator → `execution.venue` | Kafka | SimExecutionReport (simMode=true) | [messaging/sim-topics](../../06-api/messaging/sim-topics.md) |
| Simulator → `sim.execution.venue` | Kafka | SimExecutionReport (дубль) | [messaging/sim-topics](../../06-api/messaging/sim-topics.md) |
| Simulator → `sim.alerts` | Kafka | SimAlert | [messaging/sim-topics](../../06-api/messaging/sim-topics.md) |
| `execution.venue` → Ledger | Kafka | SimExecutionReport (sim-книга) | [messaging/sim-topics](../../06-api/messaging/sim-topics.md) |
| `execution.venue` → ClickHouse | Kafka→CH | sim_execution_reports | [07-data/sim-execution-reports](../../07-data/sim-execution-reports.md) |

## Data binding

| Объект | Хранилище | Doc |
|---|---|---|
| sim_sessions | PostgreSQL | [07-data/sim-sessions](../../07-data/sim-sessions.md) |
| sim_execution_reports | ClickHouse | [07-data/sim-execution-reports](../../07-data/sim-execution-reports.md) |

```mermaid
sequenceDiagram
    participant NORM as Venue MD Normalizer (F-11)
    participant K_VS as Kafka venue.snapshots
    participant SIM as VenueSimulator (F-20)
    participant VEA as Venue Execution Adapter (F-12)
    participant ROUTER as VenueSimRouter (F-20)
    participant K_EV as Kafka execution.venue
    participant K_SEV as Kafka sim.execution.venue
    participant LEDGER as Settlement Ledger
    participant CH as ClickHouse

    loop Живой LOB-поток
        NORM->>K_VS: VenueSnapshot{snapshotId, venueId, symbol, bidDepth, askDepth}
        K_VS-->>SIM: VenueSnapshot
        SIM->>SIM: Обновить LOB-кэш[venueId][symbol]
    end

    VEA->>ROUTER: ChildOrderRequest{childOrderId, hedgeFlowId, venueId, symbol, side, qty, orderType, simSessionId}
    ROUTER->>SIM: SimulateOrder(request)  %% routingMode=SIM_ONLY
    SIM->>SIM: 1. LOB-кэш[venueId][symbol]; 2. lobAge < staleLobThresholdMs
    SIM->>SIM: 3. LEVEL_BY_LEVEL matching → filledQty, VWAP avgPrice
    SIM->>SIM: 4. ImpactModel → Δp, impactBps; 5. FeeModel → fee; 6. RejectionModel
    SIM->>SIM: 7. LatencyModel → latencySampleMs
    SIM-->>SIM: async wait(latencySampleMs)
    alt lobAge OK и не reject
        SIM->>K_EV: SimExecutionReport{simMode=true, filledQty, avgPrice, fee, lobSnapshotId, lobAge, impactBps, latencySampleMs}
        SIM->>K_SEV: SimExecutionReport (дубль)
        K_EV-->>LEDGER: SimExecutionReport
        LEDGER->>LEDGER: simMode=true → sim-книга (боевые позиции не трогаются)
        K_EV-->>CH: INSERT sim_execution_reports
    else stale/reject
        SIM->>K_EV: SimExecutionReport{status=REJECTED, rejectReason=SIM_STALE_LOB|SIM_NO_LIQUIDITY|…}
        SIM->>K_EV: (алерт в sim.alerts)
    end
```
