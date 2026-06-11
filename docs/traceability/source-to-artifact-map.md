# Source to Artifact Map

Maps each source document fragment to its target docs-as-code artifact.

Machine-readable source-of-truth for components and features lives in [`specs/domain/`](../../specs/domain/) (`feature-component-map.yaml`, `code-map.yaml`, `traceability.yaml`).

## How to read

- **Source** — IN-ID of the original document in [`incoming-docs/`](../../incoming-docs/).
- **Fragment** — IN-ID-FR-NNN fragment identifier from the source's fragment-map.
- **Classification** — one of the classifications defined in [`.claude/skills/ingest-docs/SKILL.md`](../../.claude/skills/ingest-docs/SKILL.md).
- **Target Artifact** — repo path of the target file.
- **Update Type** — `created` / `merged` / `appended` / `linked`.
- **Status** — `complete` / `partial` / `unmapped-needs-review` / `blocked-conflict`.

## Table

| Source | Fragment | Classification | Target Artifact | Update Type | Status |
| --- | --- | --- | --- | --- | --- |
| IN-001 | IN-001-FR-001 | GLOSSARY | `docs/01-business/glossary.md`, `docs/04-domain/ubiquitous-language.md` | merged | complete |
| IN-001 | IN-001-FR-002 | GLOSSARY, DOMAIN_ENTITY | `docs/04-domain/ubiquitous-language.md`, `docs/04-domain/entities.md` | merged | complete |
| IN-001 | IN-001-FR-003 | GLOSSARY, DOMAIN_ENTITY | `docs/04-domain/ubiquitous-language.md`, `docs/04-domain/entities.md` | merged | complete |
| IN-001 | IN-001-FR-004 | GLOSSARY, DOMAIN_ENTITY | `docs/04-domain/ubiquitous-language.md` | merged | complete |
| IN-001 | IN-001-FR-005 | GLOSSARY | `docs/01-business/glossary.md` (Дополнения из IN-001) | appended | complete |
| IN-001 | IN-001-FR-006 | ACTOR, GLOSSARY | `docs/02-system/actors.md` | created | complete |
| IN-001 | IN-001-FR-007 | GLOSSARY, DOMAIN_ENTITY | `docs/04-domain/entities.md`, `docs/04-domain/ubiquitous-language.md` | merged | complete |
| IN-001 | IN-001-FR-008 | DOMAIN_ENTITY, DATA_MODEL | `docs/04-domain/entities.md`, `docs/07-data/oltp-schema.md`, `docs/07-data/olap-schema.md` | merged | complete |
| IN-001 | IN-001-FR-009 | DOMAIN_ENTITY, METHODOLOGY | `docs/04-domain/domain-overview.md` | linked | complete |
| IN-001 | IN-001-FR-010 | BUSINESS_REQUIREMENT | `docs/01-business/vision.md` | linked | complete |
| IN-001 | IN-001-FR-011 | BUSINESS_REQUIREMENT | `docs/01-business/goals.md` | linked | complete |
| IN-001 | IN-001-FR-012 | BUSINESS_REQUIREMENT | `docs/01-business/value-proposition.md` | linked | complete |
| IN-001 | IN-001-FR-013 | BUSINESS_REQUIREMENT | `docs/02-system/system-overview.md` (Продуктовая модель) | merged | complete |
| IN-001 | IN-001-FR-014 | ACTOR | `docs/02-system/actors.md` | created | complete |
| IN-001 | IN-001-FR-015 | USE_CASE | `docs/02-system/use-cases/UC-F02-01-*/`, `UC-F03-01-*/`, `UC-F10-01-*/` | linked | complete |
| IN-001 | IN-001-FR-016 | FUNCTIONAL_REQUIREMENT | `docs/02-system/functional-requirements.md` | created | complete |
| IN-001 | IN-001-FR-017 | NON_FUNCTIONAL_REQUIREMENT | `docs/02-system/non-functional-requirements.md` | created | complete |
| IN-001 | IN-001-FR-018 | NON_FUNCTIONAL_REQUIREMENT, OPERATIONAL_REQUIREMENT | `docs/02-system/non-functional-requirements.md` (NFR-REG), `docs/11-operations/runbook.md` | merged | partial |
| IN-001 | IN-001-FR-019 | TEST_REQUIREMENT | per-feature `Acceptance Criteria (IN-001)` блоки в `docs/02-system/features/F-*/README.md` | appended | complete |
| IN-001 | IN-001-FR-020 | BUSINESS_REQUIREMENT | `docs/01-business/constraints.md` | linked | partial |
| IN-001 | IN-001-FR-021 | EXTERNAL_SYSTEM | `docs/03-architecture/context-diagram.md` | created | complete |
| IN-001 | IN-001-FR-022 | COMPONENT | `docs/05-components/components-overview.md` + per-component overviews | linked | complete |
| IN-001 | IN-001-FR-023 | SERVICE_SEQUENCE_HINT | `docs/05-components/sequences/` (17 SEQ-FXX-UC-FXX-NN-services.md) | linked | complete |
| IN-001 | IN-001-FR-024 | CONTAINER | `docs/03-architecture/container-diagram.md` | created | complete |
| IN-001 | IN-001-FR-025 | METHODOLOGY | `docs/00-methodology/document-ingestion.md`, `docs/00-methodology/repository-map.md` | linked | complete |
| IN-001 | IN-001-FR-026 | TRACEABILITY | `docs/traceability/feature-traceability.md` | merged | complete |
| IN-001 | IN-001-FR-027 | FEATURE | `docs/02-system/features/F-01..F-17/` | linked | complete |
| IN-001 | IN-001-FR-028 | TEST_REQUIREMENT, FEATURE | per-feature `README.md` (Acceptance Criteria блок) | appended | complete |
| IN-001 | IN-001-FR-029 | DATA_MODEL | `docs/07-data/oltp-schema.md` | created | complete |
| IN-001 | IN-001-FR-030 | DATA_MODEL | `docs/07-data/olap-schema.md` | created | complete |
| IN-001 | IN-001-FR-031 | DATA_FLOW | `docs/07-data/data-flow.md` | created | complete |
| IN-003 | IN-003-FR-001 | GLOSSARY, DOMAIN_ENTITY | `docs/04-domain/entities.md`, `docs/04-domain/ubiquitous-language.md` | merged | complete |
| IN-003 | IN-003-FR-002 | DOMAIN_ENTITY, DATA_MODEL | `docs/04-domain/entities.md`, `docs/07-data/oltp-schema.md` | merged | complete |
| IN-003 | IN-003-FR-003 | KAFKA_TOPIC, DATA_MODEL | `docs/06-api/messaging/`, `docs/07-data/` | merged | complete |
| IN-003 | IN-003-FR-004 | GLOSSARY | `docs/01-business/glossary.md` | linked | complete |
| IN-003 | IN-003-FR-005 | NON_FUNCTIONAL_REQUIREMENT | `docs/02-system/non-functional-requirements.md` | merged | complete |
| IN-003 | IN-003-FR-006 | DOMAIN_ENTITY, BUSINESS_RULE | `docs/02-system/features/F-04-batch-clearing/README.md` | merged | complete |
| IN-003 | IN-003-FR-007 | FEATURE | `docs/02-system/features/F-04-batch-clearing/README.md` | merged | complete |
| IN-003 | IN-003-FR-008 | BUSINESS_REQUIREMENT | `docs/02-system/features/F-04-batch-clearing/README.md` | merged | complete |
| IN-003 | IN-003-FR-009 | USE_CASE | `docs/02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md` | merged | complete |
| IN-003 | IN-003-FR-010 | SYSTEM_SEQUENCE_HINT | `docs/02-system/use-cases/UC-F04-01-run-batch-clearing/sequences/SEQ-UC-F04-01-system.md` | linked | complete |
| IN-003 | IN-003-FR-011 | USE_CASE, SYSTEM_SEQUENCE_HINT | `docs/02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md` (Alternative Flows) | merged | complete |
| IN-003 | IN-003-FR-012 | FEATURE | `docs/02-system/features/F-04-batch-clearing/README.md` (UX/UI секция) | merged | complete |
| IN-003 | IN-003-FR-013 | TEST_REQUIREMENT | `docs/02-system/features/F-04-batch-clearing/README.md` (Acceptance Criteria) | merged | complete |
| IN-003 | IN-003-FR-014 | FUNCTIONAL_REQUIREMENT | `docs/02-system/functional-requirements.md` (FR-CLEAR) | linked | complete |
| IN-003 | IN-003-FR-015 | NON_FUNCTIONAL_REQUIREMENT | `docs/02-system/non-functional-requirements.md` | merged | complete |
| IN-003 | IN-003-FR-016 | COMPONENT | `docs/05-components/matching-fob-core/overview.md` | linked | complete |
| IN-003 | IN-003-FR-017 | SERVICE_SEQUENCE_HINT | `docs/05-components/sequences/SEQ-F04-UC-F04-01-services.md` | merged | complete |
| IN-003 | IN-003-FR-018 | CONTRACT_HINT | `docs/06-api/` | linked | complete |
| IN-003 | IN-003-FR-019 | DOMAIN_ENTITY, DATA_FLOW | `docs/04-domain/entities.md`, `docs/07-data/data-flow.md` | linked | complete |
| IN-003 | IN-003-FR-020 | IMPLEMENTATION_HINT | `docs/05-components/matching-fob-core/overview.md`, `docs/implementation-plan/F-04-batch-clearing.tasks.md` | merged | complete |
| IN-003 | IN-003-FR-021 | SERVICE_SEQUENCE_HINT, CONTRACT_HINT | `docs/05-components/matching-fob-core/sequences/SEQ-MATCHING-001-solver-cycle.md` | created | complete |
| IN-003 | IN-003-FR-022 | TEST_REQUIREMENT | `docs/10-testing/features/F-04-test-plan.md` | created | complete |
| IN-003 | IN-003-FR-023 | TEST_REQUIREMENT | `docs/10-testing/features/F-04-test-plan.md` | created | complete |
| IN-003 | IN-003-FR-024 | NON_FUNCTIONAL_REQUIREMENT | `docs/02-system/non-functional-requirements.md` (Таблица 6) | merged | complete |
| IN-003 | IN-003-FR-025 | TEST_REQUIREMENT | `docs/10-testing/features/F-04-test-plan.md` | created | complete |
| IN-003 | IN-003-FR-026 | IMPLEMENTATION_HINT | `docs/implementation-plan/F-04-batch-clearing.tasks.md` (Definition of Done IN-003) | merged | complete |
| IN-011 | IN-011-ARCH-001..004 | BUSINESS_RULE, FEATURE | `docs/03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md`, `docs/02-system/features/F-09-batch-combo-orders/feature.yaml` | created | complete |
| IN-011 | IN-011-DM-001..009 | DOMAIN_ENTITY, DATA_MODEL | `docs/04-domain/entities.md`, `docs/07-data/oltp-schema.md` | merged | complete |
| IN-011 | IN-011-BR-001..008 | BUSINESS_REQUIREMENT | `docs/02-system/features/F-09-batch-combo-orders/acceptance-criteria.md`, `docs/03-architecture/adr/ADR-031..032` | created | complete |
| IN-011 | IN-011-UC-001..003 | USE_CASE, SEQUENCE_HINT | `docs/02-system/use-cases/UC-F09-0{1,2,3}-*`, `docs/05-components/sequences/SEQ-F09-UC-F09-0{1,2,3}-services.md` | created | complete |
| IN-011 | IN-011-MATH-001..006, POL-001..005, INV | DOMAIN_FORMULAS, BUSINESS_RULE | `docs/04-domain/business-rules.md` §F-09 | created | complete |
| IN-011 | IN-011-EVT-002, DATA-001..002 | KAFKA_TOPIC, DATA_MODEL | `docs/06-api/messaging/execution-groups.md`, `docs/06-api/messaging/topics.md`, `docs/07-data/oltp-schema.md`, `docs/07-data/olap-schema.md`, ADR-033 | created | complete |
| IN-011 | IN-011-ALG-002, CONTRACT | GRPC_CONTRACT | `docs/06-api/grpc/order-flow-create-combo-order.md` (+5 TODO contracts) | created | complete |
| IN-011 | IN-011-T-001..028 | TEST_REQUIREMENT | `docs/10-testing/features/F-09-test-plan.md` | created | complete |
| IN-011 | IN-011-FINAL, OOS | IMPLEMENTATION_HINT | `docs/implementation-plan/F-09-batch-combo-orders.tasks.md` | created | complete |
| IN-012 | F-01, F-02 | DOMAIN_ENTITY | `docs/04-domain/entities.md` §Continuous-Order Primitives (state variables, CurveRepresentation enum) | merged | complete |
| IN-012 | F-03, F-05..F-10 | BUSINESS_RULE | `docs/04-domain/business-rules.md` §Clearing Mechanics (R-CLR-001..008) | merged | complete |
| IN-012 | F-04 | GLOSSARY | `docs/01-business/glossary.md` (12 терминов), `docs/04-domain/ubiquitous-language.md` (mapping table) | merged | complete |
| IN-012 | F-11, F-18, F-20, F-22 | IMPLEMENTATION_HINT | `docs/09-implementation/solver-foundation.md` (new), `cpp/matching/src/domain/solver_impl.cpp` header reference | created | complete |
| IN-012 | F-12..F-17 | DOMAIN_ENTITY | `docs/04-domain/entities.md` §1D agents (4 classes), §2D agents (8 classes) | merged | complete |
| IN-012 | F-19 | TEST_REQUIREMENT | `cpp/matching/tests/domain/continuous_market_replica_test.cpp` (new) + CMake registration | created | complete |
| IN-012 | F-21 (5 conclusions) | METHODOLOGY | `docs/03-architecture/adr/ADR-035-fob-solver-mathematical-foundation.md` (new) — context + decision + alternatives + 3 open questions | created | complete |
| IN-012 | (cross-feature linking) | FEATURE_LINK | `docs/02-system/features/F-04-batch-clearing/feature.yaml` §theoreticalFoundation, `F-10-mm-curves` §theoreticalFoundation, `F-11-external-venues-lob-to-fob` §theoreticalFoundation | merged | complete |

> Полная карта фрагментов IN-011 (все §1–§21 + Доп. пояснения) — в [`incoming-docs/IN-011.fragment-map.md`](../../incoming-docs/IN-011.fragment-map.md).
> Полная карта фрагментов IN-012 (все F-01..F-24) — в [`incoming-docs/IN-012.fragment-map.md`](../../incoming-docs/IN-012.fragment-map.md).

| IN-013 | F-01, F-02, F-06 | METHODOLOGY, REPO_STRUCTURE | `docs/00-methodology/functional-hierarchy-and-decomposition.md` (new) §1–§3 (two-axis model + Cockburn levels + mapping IN-013↔repo paths) | created | complete |
| IN-013 | F-03, F-08 | METHODOLOGY | `docs/00-methodology/sequence-diagram-rules.md` (L0/L1/L2 intro table) + `CLAUDE.md` §26a (level annotations + no-links-in-mermaid rule) | merged | complete |
| IN-013 | F-04, F-05, F-07, F-10 | METHODOLOGY | `docs/00-methodology/functional-hierarchy-and-decomposition.md` §4–§9 (bidirectional links, frontmatter, checklists, prohibitions) | created | complete |
| IN-013 | F-05 (templates) | METHODOLOGY | `docs/00-methodology/artifact-templates.md` §Decomposition level + `02-system/features/_template/feature.yaml` + `02-system/use-cases/_template/use-case.md` + `02-system/use-cases/_template/sequences/SEQ-UC-FXX-NN-system.md` + `05-components/sequences/_template/SEQ-FXX-UC-FXX-NN-services.md` (all with `level:` field) | merged | complete |
| IN-013 | F-06 (placement) | REPO_STRUCTURE | `CLAUDE.md` §0c (placement table with L0/L1/L2) | merged | complete |
| IN-013 | F-09 (full example) | METHODOLOGY | `docs/traceability/feature-to-uc.md` (new, all 18 features), `docs/traceability/uc-to-sequences.md` (new, ~33 UCs L0/L1/L2 inventory), `docs/traceability/sequence-to-code.md` (new, L2→cpp/) | created | complete |

> Полная карта фрагментов IN-013 (все F-01..F-10) — в [`incoming-docs/IN-013.fragment-map.md`](../../incoming-docs/IN-013.fragment-map.md).
