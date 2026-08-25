# SEQ-F20-UC-F20-02-services — SHADOW compare (service-level)

- Feature: [F-20](../../02-system/features/F-20-live-venue-simulator/README.md)
- Use case: [UC-F20-02](../../02-system/use-cases/UC-F20-02-shadow-compare/use-case.md)
- Level: L1
- Components: VenueSimRouter, VenueSimulator, Divergence Service (F-20), EVC (F-11), ClickHouse.

## Contract binding

| Стрелка | Transport | Contract | Doc |
|---|---|---|---|
| Router → EVC (LIVE fork) | gRPC | PlaceChildOrder | F-11 EVC |
| EVC → `execution.venue` | Kafka | ExecutionReport (simMode=false) | [messaging/sim-topics](../../06-api/messaging/sim-topics.md) |
| Simulator → `execution.venue` | Kafka | SimExecutionReport (simMode=true) | [messaging/sim-topics](../../06-api/messaging/sim-topics.md) |
| `execution.venue` → Divergence | Kafka | LIVE+SIM пары по clientOrderId | [messaging/sim-topics](../../06-api/messaging/sim-topics.md) |
| Divergence → ClickHouse | HTTP insert | sim_divergence_log | [07-data/sim-execution-reports](../../07-data/sim-execution-reports.md) |

```mermaid
sequenceDiagram
    participant VEA as Venue Execution Adapter (F-12)
    participant ROUTER as VenueSimRouter (F-20)
    participant EVC as External Venues Connector (F-11)
    participant VENUE as External Venue
    participant SIM as VenueSimulator (F-20)
    participant K_EV as Kafka execution.venue
    participant DIV as Divergence Service (F-20)
    participant CH as ClickHouse

    VEA->>ROUTER: ChildOrderRequest  %% routingMode=SHADOW
    par LIVE fork
        ROUTER->>EVC: PlaceChildOrder(request)
        EVC->>VENUE: REST/WS order
        VENUE-->>EVC: ExecutionReport (LIVE)
        EVC->>K_EV: ExecutionReport{simMode=false}
    and SIM fork
        ROUTER->>SIM: SimulateOrder(request)
        SIM->>K_EV: SimExecutionReport{simMode=true}
    end
    K_EV-->>DIV: Оба отчёта (по clientOrderId)
    DIV->>DIV: delta fillRate / delta price(bps) / delta latency / delta fee
    DIV->>CH: INSERT sim_divergence_log
```
