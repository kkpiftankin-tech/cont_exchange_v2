# SEQ-F09-UC-F09-01-services. Combo Order: service view

## Type

Service Interaction Sequence

## Feature

- [F-09](../../02-system/features/F-09-batch-combo-orders/)

## Use Case

- [UC-F09-01](../../02-system/use-cases/UC-F09-01-create-combo-order/use-case.md)

## Participants

- Web UI
- gateway
- order-flow
- risk-manager
- ledger
- Kafka

## Diagram

```mermaid
sequenceDiagram
    participant UI as Web UI
    participant GW as gateway
    participant OF as order-flow
    participant RISK as risk-manager
    participant LDG as ledger
    participant K as Kafka

    UI->>GW: REST POST /v1/combo-orders
    GW->>OF: gRPC CreateComboOrderRequest(legs)
    OF->>RISK: PreTradeCheck on aggregate
    RISK-->>OF: decision
    alt accepted
        OF->>LDG: Reserve(net-position)
        LDG-->>OF: ok
        OF->>K: publish orders.normalized (each leg, combo_id)
        OF-->>GW: combo accepted (legs with order_ids)
    else rejected
        OF-->>GW: rejected
    end
    GW-->>UI: response
```

## Contract Binding Table

| Step | Transport | Contract | Location |
| --- | --- | --- | --- |
| UI → GW | REST | `POST /v1/combo-orders` (planned) | [../../06-api/rest/](../../06-api/rest/) |
| GW → OF | gRPC | `OrderFlowService/CreateComboOrder` (planned) | [../../06-api/grpc/order-flow-create-combo-order.md](../../06-api/grpc/order-flow-create-combo-order.md) |
| OF → Kafka | Kafka | `orders.normalized` (combo_id tag) | [../../06-api/messaging/orders-normalized.md](../../06-api/messaging/orders-normalized.md) |

## Data Binding Table

| Data Object | Storage | Location |
| --- | --- | --- |
| `flow_orders` (combo_id) | PostgreSQL | [../../07-data/data-overview.md](../../07-data/data-overview.md) |

## Related Components

- [gateway](../gateway/overview.md)
- [order-flow](../order-flow/overview.md)
- [risk-manager](../risk-manager/overview.md)
- [ledger](../ledger/overview.md)
- [matching-fob-core](../matching-fob-core/overview.md)
