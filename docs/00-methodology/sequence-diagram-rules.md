# Sequence Diagram Rules

## Purpose

Sequence diagrams describe behavior over time.

The project uses two mandatory types of sequence diagrams:

1. System-level sequence diagrams.
2. Service-level sequence diagrams.

These rules are also expressed in `CLAUDE.md` §26a; this file is the canonical, version-controlled prose reference.

## Type A: System-level Sequence Diagram

### Location

`docs/02-system/use-cases/{UC-ID}/sequences/SEQ-{UC-ID}-system.md`

### Purpose

Shows external actors interacting with the Continuous Exchange System as a black box.

### Allowed Participants

- Trader
- Market Maker
- Provider
- Operator
- CEX
- DEX
- Custody
- KYC/AML Provider
- Regulator
- Continuous Exchange System

### Forbidden Participants

- API Gateway
- Auth & Identity
- Matching Backend
- Risk Manager
- Kafka
- PostgreSQL
- ClickHouse
- internal service names

## Type B: Service-level Sequence Diagram

### Location

`docs/05-components/sequences/SEQ-{F-ID}-{UC-ID}-services.md`

### Purpose

Shows how internal services implement a use case.

### Allowed Participants

- Web UI
- API Gateway (`gateway`)
- Auth & Identity (`auth-identity`, planned)
- Order Flow (`order-flow`)
- Matching Backend (`matching-fob-core`)
- Risk Manager (`risk-manager`)
- Collateral & Ledger (`ledger`)
- Market Data Service (`market-data`)
- External Venues Connector (`external-venues`)
- Backtest & Replay Engine (`backtest-replay`, planned)
- Observability & Reporting (`observability-reporting`)
- Kafka
- PostgreSQL
- ClickHouse
- Redis
- WebSocket Gateway (planned)
- Blockchain / Custody Adapter (planned)

## Required Sections for System-level Sequence

- Type
- Feature
- Use Case
- Purpose
- Participants
- Diagram
- External Message Table (planned addition to existing template)
- Related Service Sequence
- Source Fragments (planned addition)

Existing template: [`docs/02-system/use-cases/_template/sequences/SEQ-UC-FXX-NN-system.md`](../02-system/use-cases/_template/sequences/SEQ-UC-FXX-NN-system.md).

## Required Sections for Service-level Sequence

- Type
- Feature
- Use Case
- Purpose
- Participants
- Diagram
- Contract Binding Table
- Data Binding Table
- Produced Events
- Consumed Events
- Related Components
- Related Contracts
- Related Data Objects
- Source Fragments (planned addition)

Existing template: [`docs/05-components/sequences/_template/SEQ-FXX-UC-FXX-NN-services.md`](../05-components/sequences/_template/SEQ-FXX-UC-FXX-NN-services.md).

## Mermaid Rule

Use Mermaid `sequenceDiagram`.

Do not use `graph`, `flowchart`, or informal arrows as the primary interaction artifact.

## Contract Rule

Every service-level arrow must have:

- transport;
- contract/message name;
- target contract document.

If the contract does not exist, create TODO contract documentation in `docs/06-api/`.

## Data Rule

Every SQL or persistence step must link to:

- data object;
- storage;
- target schema document.

Example target:

`docs/07-data/oltp-schema.md#flow_orders`

While the OLTP/OLAP schema documents are planned, link to [`docs/07-data/data-overview.md`](../07-data/data-overview.md) until they exist.
