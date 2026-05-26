# Incoming Documents Index

Immutable archive of incoming source documents. Each entry receives an `IN-XXX` ID. Originals are never edited; if a corrected version arrives, register it as a new entry and link from the previous meta file.

Ingestion workflow: [`.claude/skills/ingest-docs/SKILL.md`](../.claude/skills/ingest-docs/SKILL.md).

| ID | File | Date | Type | Status | Processed Into |
| --- | --- | ---: | --- | --- | --- |
| IN-001 | [2026-05-13-EXPLANATORY-NOTE-full.md](2026-05-13-EXPLANATORY-NOTE-full.md) (полный архив) + [EXPLANATORY-NOTE-source.md](EXPLANATORY-NOTE-source.md) (historical placeholder) | 2026-05-13 | Explanatory note (Пояснительная записка) | ingested | `01-business`, `02-system`, `03-architecture`, `04-domain`, `05-components`, `06-api/messaging`, `07-data`. Meta: [IN-001.meta.md](IN-001.meta.md). Fragments: [IN-001.fragment-map.md](IN-001.fragment-map.md) |
| IN-002 | [2026-05-13-Этапы.md](2026-05-13-Этапы.md) | 2026-05-13 | Methodology (Этапы) | partially-ingested | `CLAUDE.md`, `docs/00-methodology/`, `docs/02-system/features/`, `docs/05-components/sequences/` |
| IN-003 | [2026-05-13-F-04-Batch-Clearing-v1.md](2026-05-13-F-04-Batch-Clearing-v1.md) | 2026-05-13 | F-04 detailed spec | ingested | `02-system/features/F-04-*`, `02-system/use-cases/UC-F04-01-*`, `05-components/matching-fob-core/sequences` (new), `05-components/sequences/SEQ-F04-*`, `10-testing/features/F-04-test-plan.md` (new), `02-system/non-functional-requirements.md`, `implementation-plan/F-04-*`. Meta: [IN-003.meta.md](IN-003.meta.md). Fragments: [IN-003.fragment-map.md](IN-003.fragment-map.md) |
| IN-004 | [2026-05-20-F-11-external-venues-v1.md](2026-05-20-F-11-external-venues-v1.md) | 2026-05-20 | F-11 detailed spec (External Venues / LOB-to-FOB) | new | `02-system/features/F-11-external-venues-lob-to-fob/*`, `02-system/use-cases/UC-F11-*`, `05-components/external-venues-connector/*`, `05-components/venue-liquidity-curve-builder/*`, `05-components/venue-health-routing/*`, `05-components/sequences/SEQ-F11-*`, `06-api/messaging/venue-topics.md`, `06-api/rest/venues.md`, `07-data/venue-config.md`, `07-data/venue-snapshots.md`, `10-testing/features/F-11-test-plan.md`. Meta: [IN-004.meta.md](IN-004.meta.md). Fragments: [IN-004.fragment-map.md](IN-004.fragment-map.md) |
| IN-005 | [2026-05-20-F-12-execution-hedge-v1.md](2026-05-20-F-12-execution-hedge-v1.md) | 2026-05-20 | F-12 detailed spec (Execution Hedge) | superseded-by-IN-008 | `02-system/features/F-12-execution-hedge/*`, `02-system/use-cases/UC-F12-*`, `05-components/venue-execution-adapter/*`, `05-components/execution-planning/*`, `05-components/sequences/SEQ-F12-*`, `06-api/messaging/execution-topics.md`, `06-api/rest/hedgeflows.md`, `07-data/hedgeflows.md`, `07-data/child-orders.md`, `07-data/execution-reports.md`, `10-testing/features/F-12-test-plan.md`. Meta: [IN-005.meta.md](IN-005.meta.md). Fragments: [IN-005.fragment-map.md](IN-005.fragment-map.md) |
| IN-006 | [2026-05-20-F-15-backtest-replay-v1.md](2026-05-20-F-15-backtest-replay-v1.md) | 2026-05-20 | F-15 detailed spec (Backtest/Replay) | ingested | `02-system/features/F-15-backtest-replay/*`, `02-system/use-cases/UC-F15-{01..06}-*`, `05-components/backtest-service/*`, `05-components/sequences/SEQ-F15-{01..04}-*`, `06-api/rest/replay.md`, `06-api/messaging/replay-topics.md`, `07-data/replay-sessions.md`, `07-data/replay-agentlogs.md`, `07-data/replay-summaries.md`, `07-data/replay-rbac.md`, `10-testing/features/F-15-test-plan.md`, `implementation-plan/F-15-*`, `03-architecture/adr/ADR-009..011`, `04-domain/business-rules.md §F-15`. Meta: [IN-006.meta.md](IN-006.meta.md). Fragments: [IN-006.fragment-map.md](IN-006.fragment-map.md) |
| IN-007 | [2026-05-22-runtime-findings-orderflow-execution.md](2026-05-22-runtime-findings-orderflow-execution.md) | 2026-05-22 | Runtime findings (F-04/F-12 integration gaps) | resolved-in-pr-12 | `02-system/features/F-04-batch-clearing/feature.yaml#knownIssues` (BUG-1, BUG-2), `02-system/features/F-12-execution-hedge/feature.yaml#knownIssues` (BUG-3), `04-domain/business-rules.md`, `CLAUDE.md §7.3`, `06-api/messaging/execution-reports.md`. Meta: [IN-007.meta.md](IN-007.meta.md) |
| IN-008 | chat-attached (mojibake; см. IN-008.meta.md §notes_on_archival) | 2026-05-23 | F-12 detailed spec v2 (Execution Hedge, supersedes IN-005) | superseded-by-IN-009 | `02-system/features/F-12-execution-hedge/feature.yaml#definitionOfDone` (new section, 18 items), `02-system/features/F-12-execution-hedge/feature.yaml#uxScreens` (new section, 6 screens), `implementation-plan/F-12-execution-hedge.tasks.md` (source-link). Meta: [IN-008.meta.md](IN-008.meta.md) |
| IN-009 | chat-attached "F-12-Execution-Hedge-FINAL (1).md" (mojibake) | 2026-05-25 | F-12 detailed spec v2.0-final (adds Section 7 Observability Reporting) | ingested-as-deltas | `02-system/features/F-12-execution-hedge/feature.yaml#architecture` (new), `feature.yaml#uxScreens` (extended with refresh_mode+data_sources), `feature.yaml#operatorTimeline` (new), `feature.yaml#knownIssues` (observability-reporting-vs-frontend-api). Meta: [IN-009.meta.md](IN-009.meta.md) |

## Status values

- `new` — registered, not segmented yet
- `segmenting` — fragment-map being created
- `partially-ingested` — fragments mapped but coverage not complete
- `ingested` — all fragments mapped, traceability complete
- `superseded-by-IN-NNN` — replaced by a newer version
- `ingested-as-deltas` — only delta-changes applied to existing target docs (no full ingest needed)
- `resolved-in-pr-NN` — runtime findings closed in a specific PR

## Per-document files

For each registered document, the ingestion skill produces:

- `incoming-docs/{IN-ID}.meta.md` — metadata, type, processing status, target areas
- `incoming-docs/{IN-ID}.fragment-map.md` — fragments with classification and target artifacts

IN-001 имеет полный набор meta + fragment-map. Для IN-002 (Этапы) ingestion ещё не запускался отдельно — методология применена непосредственно (создан `ЭТАПЫ.md` в корне + структура `docs/00-methodology/`).
