# Implementation Plan

This directory contains implementation task files generated from completed documentation artifacts.

Implementation tasks must be derived from:

- feature documents;
- use cases;
- system-level sequence diagrams;
- service-level sequence diagrams;
- contracts;
- data schemas;
- acceptance criteria.

Do not generate or modify application code before the required documentation chain exists.

Required chain:

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

## Naming convention

```
docs/implementation-plan/F-XX-short-name.tasks.md
```

One file per feature. See template in [`../00-methodology/artifact-templates.md`](../00-methodology/artifact-templates.md#implementation-task-template).

## Pre-conditions per file

Each tasks file MUST begin with a checklist that confirms the documentation chain is in place. Tasks may only be started after all checks pass:

- [ ] Feature exists
- [ ] Use case exists
- [ ] System-level sequence exists
- [ ] Service-level sequence exists
- [ ] Contracts exist (no `TODO contract` left without backing)
- [ ] Data objects exist
- [ ] Acceptance criteria exist

## Related

- Coverage matrix: [`../traceability/coverage-matrix.md`](../traceability/coverage-matrix.md)
- Feature index: [`../02-system/features/feature-index.md`](../02-system/features/feature-index.md)
- Ingestion skill: [`../../.claude/skills/ingest-docs/SKILL.md`](../../.claude/skills/ingest-docs/SKILL.md)
