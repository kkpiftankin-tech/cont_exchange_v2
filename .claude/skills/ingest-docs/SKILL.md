---
description: Process incoming project documentation into the repository docs-as-code structure. Use when the user asks to insert, classify, ingest, normalize, distribute, trace, or formalize attached documentation, including features, use cases, sequence diagrams, contracts, data models, and implementation tasks.
---

# Skill: ingest-docs

## Purpose

Transform incoming documentation into structured, linked, traceable docs-as-code artifacts for the Continuous Code Repository Exchange / Continuous Flow Order Book project.

Do not merely copy source documents. Convert them into normalized repository artifacts.

## Mandatory Pipeline

For each incoming document, execute:

1. Register
2. Segment
3. Classify
4. Map
5. Normalize
6. Insert or merge
7. Link bidirectionally
8. Validate coverage
9. Generate implementation tasks

## Source Archive Rule

All incoming documents must first be preserved under:

- `incoming-docs/`

Incoming source files are immutable.

If the user provides an updated version, create a new file. Do not edit the original archived source.

## Register

Update or create:

- `incoming-docs/index.md`

For each source document, assign an ID:

- `IN-001`
- `IN-002`
- `IN-003`

Create:

- `incoming-docs/{IN-ID}.meta.md`

Required sections:

- Source File
- Document Type
- Processing Status
- Target Areas
- Notes

## Segment

Split the source document into fragments.

Create:

- `incoming-docs/{IN-ID}.fragment-map.md`

Each fragment must have:

- fragment ID;
- source heading or source range;
- classification;
- target artifact;
- status.

## Classify

Use exactly these classifications:

- `METHODOLOGY`
- `REPO_STRUCTURE`
- `GLOSSARY`
- `BUSINESS_REQUIREMENT`
- `FUNCTIONAL_REQUIREMENT`
- `NON_FUNCTIONAL_REQUIREMENT`
- `ACTOR`
- `EXTERNAL_SYSTEM`
- `FEATURE`
- `USE_CASE`
- `SYSTEM_SEQUENCE_HINT`
- `SERVICE_SEQUENCE_HINT`
- `FID_CHAIN`
- `COMPONENT`
- `CONTAINER`
- `DOMAIN_ENTITY`
- `DOMAIN_EVENT`
- `BUSINESS_RULE`
- `CONTRACT_HINT`
- `REST_CONTRACT`
- `GRPC_CONTRACT`
- `KAFKA_TOPIC`
- `DATA_MODEL`
- `DATA_FLOW`
- `TEST_REQUIREMENT`
- `OPERATIONAL_REQUIREMENT`
- `IMPLEMENTATION_HINT`

If a fragment fits several categories, assign all relevant categories.

## Placement Matrix

Project uses the 11-folder layout (`docs/09-implementation/`, `docs/10-testing/`, `docs/11-operations/`). The instruction text uses a 10-folder layout — paths below are adapted to the **actual** repository layout. See **Conflict Note** in repository CLAUDE.md.

- `METHODOLOGY` → `docs/00-methodology/`
- `REPO_STRUCTURE` → `CLAUDE.md`, `docs/00-methodology/repository-map.md`
- `GLOSSARY` → `docs/01-business/glossary.md`, `docs/04-domain/ubiquitous-language.md`
- `BUSINESS_REQUIREMENT` → `docs/01-business/vision.md`, `docs/01-business/goals.md`
- `FUNCTIONAL_REQUIREMENT` → `docs/02-system/functional-requirements.md`
- `NON_FUNCTIONAL_REQUIREMENT` → `docs/02-system/non-functional-requirements.md`
- `ACTOR` → `docs/02-system/actors.md`
- `EXTERNAL_SYSTEM` → `docs/02-system/actors.md`, `docs/03-architecture/context-diagram.md`
- `FEATURE` → `docs/02-system/features/F-XX-short-name/` (existing convention: directory with `feature.yaml` + `README.md`)
- `USE_CASE` → `docs/02-system/use-cases/UC-FXX-YY-short-name/use-case.md`
- `SYSTEM_SEQUENCE_HINT` → `docs/02-system/use-cases/{UC-ID}/sequences/SEQ-{UC-ID}-system.md`
- `SERVICE_SEQUENCE_HINT` → `docs/05-components/sequences/SEQ-{F-ID}-{UC-ID}-services.md`
- `FID_CHAIN` → `docs/05-components/sequences/`, `docs/06-api/`, `docs/07-data/data-flow.md`
- `COMPONENT` → `docs/05-components/{component-name}/`
- `CONTAINER` → `docs/03-architecture/container-diagram.md`
- `DOMAIN_ENTITY` → `docs/04-domain/entities.md`
- `DOMAIN_EVENT` → `docs/04-domain/events/`
- `BUSINESS_RULE` → `docs/04-domain/business-rules.md`
- `CONTRACT_HINT` → `docs/06-api/` as TODO contract
- `REST_CONTRACT` → `docs/06-api/rest/`
- `GRPC_CONTRACT` → `docs/06-api/grpc/`
- `KAFKA_TOPIC` → `docs/06-api/messaging/`
- `DATA_MODEL` → `docs/07-data/oltp-schema.md`, `docs/07-data/olap-schema.md`
- `DATA_FLOW` → `docs/07-data/data-flow.md`
- `TEST_REQUIREMENT` → `docs/10-testing/` (note: 10, not 09, per project layout)
- `OPERATIONAL_REQUIREMENT` → `docs/11-operations/` (note: 11, not 10, per project layout)
- `IMPLEMENTATION_HINT` → `docs/implementation-plan/` (and may also touch `docs/09-implementation/` for code-side notes)

## Normalize

Never paste raw text directly unless it is already in target format.

Normalize each artifact using templates from:

- `docs/00-methodology/artifact-templates.md`
- existing repository templates: `docs/02-system/use-cases/_template/`, `docs/05-components/sequences/_template/`, `docs/02-system/features/_template/`

## Insert or Merge

When target file does not exist:

- create it from the appropriate template.

When target file exists:

- read it first;
- preserve existing content;
- merge into the appropriate section;
- add missing sections only when needed;
- append source fragment references.

Never delete existing content unless it is clearly duplicated and the new version is better structured.

## Build Bidirectional Links

Every feature must link to:

- requirements;
- use cases;
- system-level sequence diagrams;
- service-level sequence diagrams;
- components;
- contracts;
- data objects;
- tests.

Every use case must link to:

- feature;
- actors;
- system-level sequence;
- service-level sequence;
- contracts;
- components;
- data objects;
- acceptance criteria.

Every service-level sequence must link to:

- feature;
- use case;
- components;
- contracts;
- Kafka topics;
- database tables;
- produced/consumed events.

Every contract must link back to:

- features;
- use cases;
- sequence diagrams;
- producer/caller;
- consumer/callee;
- related component;
- related data object.

Every component must link back to:

- features;
- use cases;
- sequence diagrams;
- owned contracts;
- consumed contracts;
- produced events;
- consumed events;
- data access.

Every data object must link back to:

- owner service;
- writer services;
- reader services;
- features;
- use cases;
- sequence diagrams.

## Maintain Traceability Files

Create or update:

- `docs/traceability/source-to-artifact-map.md`
- `docs/traceability/coverage-matrix.md`
- `docs/traceability/feature-traceability.md`

Machine-readable source of truth for component/feature/contract maps lives in `specs/domain/*.yaml`. Keep markdown traceability in sync with it.

## Contract TODOs

If a service-level sequence needs a missing contract, create a TODO contract file instead of leaving an unlinked name.

Examples:

- `docs/06-api/grpc/orders.proto.md`
- `docs/06-api/grpc/risk.proto.md`
- `docs/06-api/messaging/orders-normalized.md` (already exists)
- `docs/06-api/messaging/batch-outputs.md` (already exists)

## Conflict Handling

If source text conflicts with existing docs, add `Conflict Notes` to the affected target artifact.

If the conflict changes architecture, create:

- `docs/03-architecture/adr/ADR-XXX-short-title.md`

## Generate Implementation Tasks, Not Code

After documentation is inserted and linked, generate implementation tasks.

Create:

- `docs/implementation-plan/F-XX-short-name.tasks.md`

Do not modify `cpp/` or `src/` until implementation tasks exist and the user explicitly asks for implementation.

## Current Attached Documents Mapping

For `Пояснительная записка.md` (registered as `IN-001`):

- glossary and concepts → `docs/01-business/glossary.md`, `docs/04-domain/ubiquitous-language.md`
- business goals and product model → `docs/01-business/vision.md`, `docs/01-business/goals.md`
- roles and external participants → `docs/02-system/actors.md`, `docs/03-architecture/context-diagram.md`
- requirements → `docs/02-system/functional-requirements.md`, `docs/02-system/non-functional-requirements.md`
- system components → `docs/05-components/components-overview.md`, `docs/05-components/{component}/overview.md`
- FID chains → service-level sequence diagrams in `docs/05-components/sequences/`
- containers → `docs/03-architecture/container-diagram.md`
- feature catalogue → `docs/02-system/features/`
- feature scenarios → `docs/02-system/use-cases/`
- PostgreSQL and ClickHouse tables → `docs/07-data/oltp-schema.md`, `docs/07-data/olap-schema.md`
- Kafka topic hints → `docs/06-api/messaging/`
- traceability tables → `docs/traceability/feature-traceability.md`

For `Этапы.md` (registered as `IN-002`):

- development methodology → `docs/00-methodology/development-methodology.md`
- repository structure → `docs/00-methodology/repository-map.md`
- docs-as-code rules → `docs/00-methodology/docs-as-code-rules.md`
- Claude Code working rules → root `CLAUDE.md`
- repeatable ingestion workflow → `.claude/skills/ingest-docs/SKILL.md`

## Definition of Done

The ingestion task is complete only when:

- incoming document is registered;
- meta file exists;
- fragment map exists;
- all fragments are classified;
- all fragments are mapped;
- target docs are created or updated;
- bidirectional links are added;
- traceability files are updated;
- coverage matrix is updated;
- TODO contracts are created where needed;
- implementation tasks are created;
- no source fragment is left without status.
