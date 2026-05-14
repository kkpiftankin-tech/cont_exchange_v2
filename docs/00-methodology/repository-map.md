# Repository Map

## Conflict Note vs. Instruction Text

The bootstrap instruction text uses a 10-folder layout (`09-testing`, `10-operations`). This repository uses an **11-folder layout** approved by the project owner:

| Phase | Path | Purpose |
| --- | --- | --- |
| 09 | `docs/09-implementation/` | Implementation notes, shared lib (cpp-common), migration from legacy |
| 10 | `docs/10-testing/` | Test strategy, acceptance, integration, E2E |
| 11 | `docs/11-operations/` | Runbooks, on-call, SLO, incidents |

The mapping below uses the actual 11-folder layout.

## Stable Documentation Structure

- `docs/00-methodology/` — methodology, ingestion workflow, repository rules, templates.
- `docs/01-business/` — why the system exists: vision, goals, stakeholders, glossary.
- `docs/02-system/` — what the system does: actors, requirements, features, use cases.
- `docs/03-architecture/` — how the system is structured at high level: C4, containers, ADR.
- `docs/04-domain/` — domain model: entities, value objects, domain events, invariants.
- `docs/05-components/` — services, responsibilities, dependencies, sequence diagrams.
- `docs/06-api/` — REST, gRPC, Kafka and message contracts.
- `docs/07-data/` — OLTP, OLAP, data flow, retention, migrations.
- `docs/08-infrastructure/` — deployment, configuration, CI/CD, observability infrastructure.
- `docs/09-implementation/` — implementation details: shared libs, migration map, per-service notes.
- `docs/10-testing/` — test strategy, acceptance criteria, unit/integration/E2E/performance tests.
- `docs/11-operations/` — runbooks, incidents, onboarding, operational procedures.
- `docs/traceability/` — source-to-artifact maps and coverage matrices.
- `docs/implementation-plan/` — implementation tasks derived from complete documentation.

## Incoming Documents

Incoming documents are stored in:

- `incoming-docs/`

Original incoming documents are immutable. Index: [`incoming-docs/index.md`](../../incoming-docs/index.md).

## Features

Features are stored in:

- `docs/02-system/features/F-XX-short-name/` (directory with `feature.yaml` + `README.md` + extras)

Index: [`docs/02-system/features/feature-index.md`](../02-system/features/feature-index.md).

Do not create:

- `docs/05-features/`

## Use Cases

Use cases are stored in:

- `docs/02-system/use-cases/UC-FXX-YY-short-name/use-case.md`

Template: [`docs/02-system/use-cases/_template/use-case.md`](../02-system/use-cases/_template/use-case.md).

## Sequence Diagrams

System-level sequence diagrams:

- `docs/02-system/use-cases/{UC-ID}/sequences/SEQ-{UC-ID}-system.md`

Service-level cross-component sequence diagrams:

- `docs/05-components/sequences/SEQ-{F-ID}-{UC-ID}-services.md`

Index: [`docs/05-components/sequences/README.md`](../05-components/sequences/README.md).

Internal component sequence diagrams:

- `docs/05-components/{component-name}/sequences/`

Full rules: [sequence-diagram-rules.md](sequence-diagram-rules.md).

## Contracts

REST contracts:

- `docs/06-api/rest/`

gRPC contracts:

- `docs/06-api/grpc/`

Kafka and message contracts:

- `docs/06-api/messaging/`

## Data

OLTP:

- `docs/07-data/oltp-schema.md` (planned)

OLAP:

- `docs/07-data/olap-schema.md` (planned)

Data flow:

- `docs/07-data/data-flow.md` (planned)

Current overview: [`docs/07-data/data-overview.md`](../07-data/data-overview.md).

## Traceability Chain

Business Requirement
→ Feature
→ Use Case
→ System Sequence
→ Service Sequence
→ Contracts
→ Data Objects
→ Components
→ Tests
→ Code

Machine-readable maps live in [`specs/domain/`](../../specs/domain/):

- `feature-component-map.yaml`
- `code-map.yaml`
- `traceability.yaml`

Markdown traceability lives in [`docs/traceability/`](../traceability/).
