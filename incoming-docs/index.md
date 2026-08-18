# Incoming Documents Index

Immutable archive of incoming source documents. Each entry receives an `IN-XXX` ID. Originals are never edited; if a corrected version arrives, register it as a new entry and link from the previous meta file.

Ingestion workflow: [`.claude/skills/ingest-docs/SKILL.md`](../.claude/skills/ingest-docs/SKILL.md).

| ID | File | Date | Type | Status | Processed Into |
| --- | --- | ---: | --- | --- | --- |
| IN-001 | [2026-05-13-EXPLANATORY-NOTE-full.md](2026-05-13-EXPLANATORY-NOTE-full.md) (полный архив) + [EXPLANATORY-NOTE-source.md](EXPLANATORY-NOTE-source.md) (historical placeholder) | 2026-05-13 | Explanatory note (Пояснительная записка) | ingested | `01-business`, `02-system`, `03-architecture`, `04-domain`, `05-components`, `06-api/messaging`, `07-data`. Meta: [IN-001.meta.md](IN-001.meta.md). Fragments: [IN-001.fragment-map.md](IN-001.fragment-map.md) |
| IN-002 | [2026-05-13-Этапы.md](2026-05-13-Этапы.md) | 2026-05-13 | Methodology (Этапы) | partially-ingested | `CLAUDE.md`, `docs/00-methodology/`, `docs/02-system/features/`, `docs/05-components/sequences/` |
| IN-003 | [2026-05-13-F-04-Batch-Clearing-v1.md](2026-05-13-F-04-Batch-Clearing-v1.md) | 2026-05-13 | F-04 detailed spec | ingested | `02-system/features/F-04-*`, `02-system/use-cases/UC-F04-01-*`, `05-components/matching-fob-core/sequences` (new), `05-components/sequences/SEQ-F04-*`, `10-testing/features/F-04-test-plan.md` (new), `02-system/non-functional-requirements.md`, `implementation-plan/F-04-*`. Meta: [IN-003.meta.md](IN-003.meta.md). Fragments: [IN-003.fragment-map.md](IN-003.fragment-map.md) |
| IN-004 | [F-02 Create FlowOrder v2.md](F-02%20Create%20FlowOrder%20v2.md) | 2026-08-18 | F-02 spec v2 (update) | segmenting | Meta: [IN-004.meta.md](IN-004.meta.md) · Fragments: [IN-004.fragment-map.md](IN-004.fragment-map.md) |
| IN-005 | [F‑04. Batch Clearing v1.md](F%E2%80%9104.%20Batch%20Clearing%20v1.md) | 2026-08-18 | F-04 spec v1 (clean source; dup of IN-003) | segmenting | Meta: [IN-005.meta.md](IN-005.meta.md) · Fragments: [IN-005.fragment-map.md](IN-005.fragment-map.md) |
| IN-006 | [F‑06. Просмотр позиций, PnL и маржи.md](F%E2%80%9106.%20%D0%9F%D1%80%D0%BE%D1%81%D0%BC%D0%BE%D1%82%D1%80%20%D0%BF%D0%BE%D0%B7%D0%B8%D1%86%D0%B8%D0%B9%2C%20PnL%20%D0%B8%20%D0%BC%D0%B0%D1%80%D0%B6%D0%B8.md) | 2026-08-18 | F-06 spec (source) | segmenting | Meta: [IN-006.meta.md](IN-006.meta.md) · Fragments: [IN-006.fragment-map.md](IN-006.fragment-map.md) |
| IN-007 | [F-11_corrected v1.md](F-11_corrected%20v1.md) | 2026-08-18 | F-11 spec corrected v1 (update) | segmenting | Meta: [IN-007.meta.md](IN-007.meta.md) · Fragments: [IN-007.fragment-map.md](IN-007.fragment-map.md) |
| IN-008 | [F-12-Execution-Hedge-FINAL (1).md](F-12-Execution-Hedge-FINAL%20%281%29.md) | 2026-08-18 | F-12 spec FINAL (update) | segmenting | Meta: [IN-008.meta.md](IN-008.meta.md) · Fragments: [IN-008.fragment-map.md](IN-008.fragment-map.md) |
| IN-009 | [F-15. Backtest   Replay v1.md](F-15.%20Backtest%20%20%20Replay%20v1.md) | 2026-08-18 | F-15 spec v1 (source) | segmenting | Meta: [IN-009.meta.md](IN-009.meta.md) · Fragments: [IN-009.fragment-map.md](IN-009.fragment-map.md) |
| IN-010 | [F-20 -Live-Venue-Simulator.md](F-20%20-Live-Venue-Simulator.md) | 2026-08-18 | **F-20 Live Venue Simulator (NEW feature)** | segmenting | Meta: [IN-010.meta.md](IN-010.meta.md) · Fragments: [IN-010.fragment-map.md](IN-010.fragment-map.md) |
| IN-011 | [continuous_exchange_report_v2.pdf](continuous_exchange_report_v2.pdf) | 2026-08-18 | System report v2 (PDF) | new | Meta: [IN-011.meta.md](IN-011.meta.md) · Fragments: [IN-011.fragment-map.md](IN-011.fragment-map.md) |
| IN-012 | [Кривые котирования MM (PDF)](%D0%9A%D1%80%D0%B8%D0%B2%D1%8B%D0%B5_%D0%BA%D0%BE%D1%82%D0%B8%D1%80%D0%BE%D0%B2%D0%B0%D0%BD%D0%B8%D1%8F_%D0%B2%D0%BD%D1%83%D1%82%D1%80%D0%B5%D0%BD%D0%BD%D0%B8%D1%85_%D0%BC%D0%B0%D1%80%D0%BA%D0%B5%D1%82_%D0%BC%D0%B5%D0%B9%D0%BA%D0%B5%D1%80%D0%BE%D0%B2_CE_%D0%B1%D0%B8%D1%80%D0%B6%D0%B5%D0%B2%D0%BE%D0%B5_%D0%B8%D0%B7%D0%B4%D0%B0%D0%BD%D0%B8%D0%B5.pdf) | 2026-08-18 | MM quoting curves → F-10 (PDF) | new | Meta: [IN-012.meta.md](IN-012.meta.md) · Fragments: [IN-012.fragment-map.md](IN-012.fragment-map.md) |

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
