# Incoming Documents Index

Immutable archive of incoming source documents. Each entry receives an `IN-XXX` ID. Originals are never edited; if a corrected version arrives, register it as a new entry and link from the previous meta file.

Ingestion workflow: [`.claude/skills/ingest-docs/SKILL.md`](../.claude/skills/ingest-docs/SKILL.md).

| ID | File | Date | Type | Status | Processed Into |
| --- | --- | ---: | --- | --- | --- |
| IN-001 | [2026-05-13-EXPLANATORY-NOTE-full.md](2026-05-13-EXPLANATORY-NOTE-full.md) (полный архив) + [EXPLANATORY-NOTE-source.md](EXPLANATORY-NOTE-source.md) (historical placeholder) | 2026-05-13 | Explanatory note (Пояснительная записка) | ingested | `01-business`, `02-system`, `03-architecture`, `04-domain`, `05-components`, `06-api/messaging`, `07-data`. Meta: [IN-001.meta.md](IN-001.meta.md). Fragments: [IN-001.fragment-map.md](IN-001.fragment-map.md) |
| IN-002 | [2026-05-13-Этапы.md](2026-05-13-Этапы.md) | 2026-05-13 | Methodology (Этапы) | partially-ingested | `CLAUDE.md`, `docs/00-methodology/`, `docs/02-system/features/`, `docs/05-components/sequences/` |
| IN-003 | [2026-05-13-F-04-Batch-Clearing-v1.md](2026-05-13-F-04-Batch-Clearing-v1.md) | 2026-05-13 | F-04 detailed spec | ingested | `02-system/features/F-04-*`, `02-system/use-cases/UC-F04-01-*`, `05-components/matching-fob-core/sequences` (new), `05-components/sequences/SEQ-F04-*`, `10-testing/features/F-04-test-plan.md` (new), `02-system/non-functional-requirements.md`, `implementation-plan/F-04-*`. Meta: [IN-003.meta.md](IN-003.meta.md). Fragments: [IN-003.fragment-map.md](IN-003.fragment-map.md) |

## Status values

- `new` — registered, not segmented yet
- `segmenting` — fragment-map being created
- `partially-ingested` — fragments mapped but coverage not complete
- `ingested` — all fragments mapped, traceability complete
- `superseded-by-IN-NNN` — replaced by a newer version

## Per-document files

For each registered document, the ingestion skill produces:

- `incoming-docs/{IN-ID}.meta.md` — metadata, type, processing status, target areas
- `incoming-docs/{IN-ID}.fragment-map.md` — fragments with classification and target artifacts

IN-001 имеет полный набор meta + fragment-map. Для IN-002 (Этапы) ingestion ещё не запускался отдельно — методология применена непосредственно (создан `ЭТАПЫ.md` в корне + структура `docs/00-methodology/`).
