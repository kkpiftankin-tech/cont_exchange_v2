# Artifact Templates

This file collects the canonical templates for the docs-as-code workflow.

For working starter files used during ingestion, see the existing template directories:

- [`docs/02-system/use-cases/_template/`](../02-system/use-cases/_template/) — use-case + system-level sequence.
- [`docs/05-components/sequences/_template/`](../05-components/sequences/_template/) — service-level sequence.
- [`docs/02-system/features/_template/`](../02-system/features/_template/) — feature.yaml + README.

Templates below extend these with sections required by the ingestion workflow (Source Fragments, External Message Table, Implementation Task structure).

## Feature Template

**Target:** `docs/02-system/features/F-XX-short-name/README.md` (+ `feature.yaml`).

```markdown
# F-XX. Feature name

## Purpose

...

## Business Value

...

## Related Business Requirements

- ...

## Related Functional Requirements

- ...

## Main Scenario

1. ...
2. ...
3. ...

## Related Use Cases

- `../use-cases/UC-FXX-01-short-name/use-case.md` (UC-FXX-01)

## Related Sequence Diagrams

### System-level

- `../use-cases/UC-FXX-01-short-name/sequences/SEQ-UC-FXX-01-system.md`

### Service-level

- `../../05-components/sequences/SEQ-FXX-UC-FXX-01-services.md`

## Related Components

- ...

## Related Contracts

- ...

## Related Data Objects

- ...

## Acceptance Criteria

- ...

## Source Fragments

- IN-XXX-FR-YYY
```

## Use Case Template

**Target:** `docs/02-system/use-cases/UC-FXX-YY-short-name/use-case.md`.

See [`docs/02-system/use-cases/_template/use-case.md`](../02-system/use-cases/_template/use-case.md). Extension (add at the end):

```markdown
## Source Fragments

- IN-XXX-FR-YYY
```

## System-level Sequence Template

**Target:** `docs/02-system/use-cases/{UC-ID}/sequences/SEQ-{UC-ID}-system.md`.

See [`docs/02-system/use-cases/_template/sequences/SEQ-UC-FXX-NN-system.md`](../02-system/use-cases/_template/sequences/SEQ-UC-FXX-NN-system.md). Extensions:

```markdown
## External Message Table

| Step | From | To | Business Message | Related Contract |
| --- | --- | --- | --- | --- |

## Source Fragments

- IN-XXX-FR-YYY
```

## Service-level Sequence Template

**Target:** `docs/05-components/sequences/SEQ-{F-ID}-{UC-ID}-services.md`.

See [`docs/05-components/sequences/_template/SEQ-FXX-UC-FXX-NN-services.md`](../05-components/sequences/_template/SEQ-FXX-UC-FXX-NN-services.md). Required sections (some present, some to add):

```markdown
## Contract Binding Table

| Step | From | To | Transport | Contract / Message | Target Doc | Status |
| --- | --- | --- | --- | --- | --- | --- |

## Data Binding Table

| Step | Service | Operation | Data Object | Storage | Target Doc | Status |
| --- | --- | --- | --- | --- | --- | --- |

## Produced Events

| Event | Topic | Producer | Consumers | Target Doc |
| --- | --- | --- | --- | --- |

## Consumed Events

| Event | Topic | Consumer | Source | Target Doc |
| --- | --- | --- | --- | --- |

## Source Fragments

- IN-XXX-FR-YYY
```

## TODO Contract Template

**Target:** `docs/06-api/{rest|grpc|messaging}/{contract-name}.md`.

```markdown
# Contract: ContractName

## Status

TODO / draft

## Purpose

...

## Transport

REST / gRPC / Kafka / WebSocket / SQL

## Producer / Caller

...

## Consumer / Callee

...

## Schema

TODO

## Used In Features

- ...

## Used In Use Cases

- ...

## Used In Sequence Diagrams

- ...

## Related Components

- ...

## Related Data Objects

- ...

## Source Fragments

- IN-XXX-FR-YYY
```

## Implementation Task Template

**Target:** `docs/implementation-plan/F-XX-short-name.tasks.md`.

```markdown
# Implementation Tasks: F-XX Feature name

## Source Artifacts

- Feature:
- Use Case:
- System Sequence:
- Service Sequence:
- Contracts:
- Data:

## Preconditions

- [ ] Feature exists
- [ ] Use case exists
- [ ] System-level sequence exists
- [ ] Service-level sequence exists
- [ ] Contracts exist
- [ ] Data objects exist
- [ ] Acceptance criteria exist

## Tasks

### T-FXX-001. Define or update contracts

Target:

- ...

Acceptance:

- contract document exists;
- schema is explicit;
- producers and consumers are listed;
- sequence diagrams reference it.

### T-FXX-002. Implement service behavior

Target:

- ...

Acceptance:

- behavior follows use case;
- service interactions follow sequence diagram;
- errors follow documented contract.

### T-FXX-003. Add tests

Target:

- ...

Acceptance:

- unit tests cover domain rules;
- integration tests cover service interactions;
- E2E test covers the use case.
```
