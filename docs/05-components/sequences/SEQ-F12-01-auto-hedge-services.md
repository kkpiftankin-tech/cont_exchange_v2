# SEQ-F12-01-auto-hedge-services. Auto Hedge After Batch: service view

## Type

Service Interaction Sequence (cross-component, F-12 happy path)

## Feature

- [F-12](../../02-system/features/F-12-execution-hedge/)

## Use Case

- [UC-F12-01](../../02-system/use-cases/UC-F12-01-auto-hedge-after-batch/use-case.md)
- [UC-F12-02](../../02-system/use-cases/UC-F12-02-manual-operator-hedge/use-case.md) (после Gateway путь идентичен)

## Purpose

Полная цепочка автоматического хеджа после batch clearing: эмиссия ExecutionIntent → routing → pre-hedge risk check → child order execution → ExecutionReport → ledger HedgePnL → ClickHouse audit.

## Participants

- matching-fob-core (publishes execution.intents)
- Kafka (`execution.intents`, `execution.venue`, `venue.liquidity.fob`, `venue.health`)
- execution-planning
- risk-manager
- venue-execution-adapter
- PostgreSQL (`hedgeflows`, `child_orders`)
- external-venues (EVC: CexWsRestAdapter / DexAmmRpcAdapter / VenueSimAdapter)
- External Venue (CEX/DEX/AMM)
- ledger
- ClickHouse (`execution_reports`)
- observability-reporting

## Diagram

```mermaid
sequenceDiagram
    participant M as matching-fob-core
    participant K1 as Kafka execution.intents
    participant PLAN as execution-planning
    participant K2 as Kafka venue.liquidity.fob / venue.health
    participant RISK as risk-manager
    participant ADP as venue-execution-adapter
    participant PG as PostgreSQL
    participant EVC as external-venues
    participant V as External Venue
    participant K3 as Kafka execution.venue
    participant LDG as ledger
    participant CH as ClickHouse
    participant OBS as observability

    Note over M: После batch clearing (F-04)
    M->>M: PositionSnapshotCalculator → netQty
    M->>M: HedgeTriggerPolicy.Evaluate (|netQty| ≥ threshold)
    M->>M: ExecutionIntentBuilder → ExecutionIntent
    M->>K1: publish ExecutionIntent (key=hedge_flow_id)
    K1-->>PLAN: consume ExecutionIntent
    K2-->>PLAN: SideLiquidityCurve + venue.health (F-11)
    PLAN->>PLAN: routing plan: qty[v] = L(v)/Sum L * targetQty
    PLAN->>RISK: gRPC PreHedgeCheck (3 checks)
    RISK-->>PLAN: OK (или REJECT → see SEQ-F12-03)
    PLAN->>ADP: ExecutionIntent + RoutingPlan
    ADP->>PG: INSERT hedgeflows (status=OPEN)
    loop per venue in routing plan
        ADP->>PG: INSERT child_orders (status=PENDING, client_order_id)
        ADP->>EVC: PlaceChildOrder(venue_id, instrument, side, qty, price, urgency)
        EVC->>V: REST/WS NewOrder OR on-chain tx
        V-->>EVC: execution event (filled / partial / reject)
        EVC-->>ADP: raw execution status
        ADP->>ADP: normalize → ExecutionReport
        ADP->>PG: UPDATE child_orders (filled_qty, avg_price, fee, status)
        ADP->>K3: publish ExecutionReport (key=hedge_flow_id)
    end
    ADP->>ADP: Reconciliation: gap = targetQty - filledQty
    alt gap ≤ threshold
        ADP->>PG: UPDATE hedgeflows (status=COMPLETED, avg_fill_price, hedge_pnl)
    else gap > threshold
        ADP->>PG: UPDATE hedgeflows (status=UNDERFILLED)
        Note over ADP: see SEQ-F12-03 (error scenarios)
    end
    K3-->>LDG: consume ExecutionReport
    LDG->>LDG: apply hedge: position += filled_qty * side; compute HedgePnL
    K3-->>CH: insert execution_reports row
    K3-->>OBS: log lifecycle, update metrics (latency, slippage, fill_rate)
```

## Contract Binding Table

| Step | Transport | Contract | Location |
| --- | --- | --- | --- |
| M → K1 | Kafka | `execution.intents` (`fob.execution.v1.ExecutionIntent`) | [../../06-api/messaging/execution-intents.md](../../06-api/messaging/execution-intents.md) |
| K2 → PLAN (in) | Kafka | `venue.liquidity.fob` (`fob.venue.v1.SideLiquidityCurve`) | [../../06-api/messaging/venue-liquidity-fob.md](../../06-api/messaging/venue-liquidity-fob.md) (F-11) |
| K2 → PLAN (in) | Kafka | `venue.health` (status, latency_ms) | [../../06-api/messaging/venue-health.md](../../06-api/messaging/venue-health.md) (F-11) |
| PLAN → RISK | gRPC | `fob.risk.v1.RiskService.PreHedgeCheck` | [../../06-api/grpc/risk-pre-hedge-check.md](../../06-api/grpc/risk-pre-hedge-check.md) |
| PLAN → ADP | internal | ExecutionIntent + RoutingPlan | (in-process / IPC TBD) |
| ADP → PG (W) | SQL | `INSERT INTO hedgeflows` | [../../07-data/hedgeflows.md](../../07-data/hedgeflows.md) |
| ADP → PG (W) | SQL | `INSERT INTO child_orders` | [../../07-data/child-orders.md](../../07-data/child-orders.md) |
| ADP → EVC | internal | `domain::VenueAdapter::PlaceOrder` | [`cpp/venues/src/domain/venue_adapter.hpp`](../../../cpp/venues/src/domain/venue_adapter.hpp) |
| EVC → V | venue SDK | CEX REST/WS, DEX RPC, AMM on-chain | (venue-specific, out of repo scope) |
| ADP → K3 | Kafka | `execution.venue` (`fob.execution.v1.ExecutionReport`) | [../../06-api/messaging/execution-venue.md](../../06-api/messaging/execution-venue.md) |
| K3 → LDG | Kafka | `execution.venue` consume | same as above |
| LDG (alt) | gRPC | `fob.ledger.v1.LedgerService.ApplyExecutionReport` | [../../06-api/grpc/ledger-apply-execution-report.md](../../06-api/grpc/ledger-apply-execution-report.md) |
| K3 → CH | Kafka engine | ClickHouse Kafka table → MergeTree MV | [../../07-data/execution-reports.md](../../07-data/execution-reports.md) |
| K3 → OBS | Kafka | observe lifecycle | (observability internal) |

## Data Binding Table

| Data Object | Storage | Location | Owner |
| --- | --- | --- | --- |
| `hedgeflows` | PostgreSQL | [../../07-data/hedgeflows.md](../../07-data/hedgeflows.md) | venue-execution-adapter |
| `child_orders` | PostgreSQL | [../../07-data/child-orders.md](../../07-data/child-orders.md) | venue-execution-adapter |
| `execution_reports` | ClickHouse | [../../07-data/execution-reports.md](../../07-data/execution-reports.md) | venue-execution-adapter (writer), F-15/F-13/F-17 (readers) |
| `positions` | PostgreSQL | [../../07-data/data-overview.md](../../07-data/data-overview.md) | ledger (F-06 owner) |

## Related Components

- [matching-fob-core](../matching-fob-core/overview.md)
- [execution-planning](../execution-planning/overview.md)
- [venue-execution-adapter](../venue-execution-adapter/overview.md)
- [risk-manager](../risk-manager/overview.md)
- [external-venues](../external-venues/overview.md)
- [ledger](../ledger/overview.md)
- [observability-reporting](../observability-reporting/overview.md)

## Source Fragments

- IN-005 §2 «Sequence diagram — основной happy path»
- IN-005 §6 «Формулы расчётов» (routing plan, pre-hedge checks)
- IN-005 §7 (F12-1..F12-12)
