# PM-002 — F-15 status drift между `feature.yaml` и `feature-component-map.yaml`

> **Type**: process post-mortem.
>
> **Status**: `published` (fix landed via [AUDIT-001 T-AUDIT-001](../audits/AUDIT-001-feature-development-process.md), 2026-06-08).
>
> **Discovered**: 2026-06-08 by external review.
>
> **Severity**: medium — traceability дашборды и автоматизация показывали ложную картину; F-15 implementation шла "вслепую" по отношению к machine-readable source-of-truth.

## Symptoms

Два YAML-источника, оба объявленные как "source of truth", показывали разное:

| Источник | F-15 status | F-15 codePaths | F-15 primaryComponents |
|---|---|---|---|
| [`docs/02-system/features/F-15-backtest-replay/feature.yaml`](../../02-system/features/F-15-backtest-replay/feature.yaml) | `in-progress-impl` | ~60 файлов под `cpp/backtest/` (явно перечислены) | `backtest-service`, `matching-fob-core`, `risk-manager`, `settlement-ledger`, `market-data` |
| [`specs/domain/feature-component-map.yaml`](../../../specs/domain/feature-component-map.yaml) (до фикса) | `not-implemented` | `[]` | `matching`, `risk`, `ledger`, `observability` |

При этом [feature-traceability.md](../../traceability/feature-traceability.md) колонка `Code` для F-15 = `—` (пусто), а [coverage-matrix.md](../../traceability/coverage-matrix.md) status = `needs-data`. Все три markdown-проекции описывали ситуацию **до начала реализации**, хотя реализация уже была в активной разработке.

## Root cause

1. **Нет валидатора согласованности feature.yaml ↔ feature-component-map.yaml.** `traceability-checker` (99 строк) проверяет только наличие путей (docsPath/codePath/protoContract существуют как файлы) — не сверяет статусы или содержимое полей между источниками.
2. **Конвенция нейминга компонентов разная между двумя файлами**: `feature.yaml` использует архитектурные псевдонимы (`backtest-service`, `matching-fob-core`), а `feature-component-map.yaml` использует короткие C++ service names (`matching`, `backtest`). Никто не зафиксировал, как они мапятся.
3. **Owner-of-record не задокументирован**. В [ingest-and-agents-integration.md §5](../ingest-and-agents-integration.md) есть owner для feature.yaml (system-analyst через ingress-engineer), но кто и когда апдейтит `specs/domain/feature-component-map.yaml` — не зафиксировано. На практике это делается ad-hoc.

## Fix

| Action | Owner task |
|---|---|
| Sync F-15 entry в [feature-component-map.yaml](../../../specs/domain/feature-component-map.yaml) — статус `in-progress-impl`, добавлены 8 proto-контрактов, codePaths, kafkaTopics, primary backtest component с aliases в комментариях | [AUDIT-001 T-AUDIT-001](../audits/AUDIT-001-feature-development-process.md), 2026-06-08 |
| Обновить [feature-traceability.md F-15](../../traceability/feature-traceability.md) — actual status вместо `—` | T-AUDIT-001 |
| Обновить [coverage-matrix.md F-15](../../traceability/coverage-matrix.md) — `in-progress` вместо `needs-data` | T-AUDIT-001 |
| Создать [`tools/feature-yaml-checker/`](../../../tools/) — кросс-валидатор feature.yaml ↔ feature-component-map.yaml | [T-AUDIT-006](../audits/AUDIT-001-feature-development-process.md) |
| Подключить feature-check в Makefile и CI | [T-AUDIT-008](../audits/AUDIT-001-feature-development-process.md), [T-AUDIT-009](../audits/AUDIT-001-feature-development-process.md) (Phase 2) |

## Lessons learned

- **Два source-of-truth ⇒ обязательный кросс-валидатор.** Markdown — для человека, YAML — для скриптов. Drift между ними неизбежен, если CI его не ловит.
- **Каждый "канонический" файл должен иметь зафиксированного owner-а**. `ingest-and-agents-integration.md §5` — хороший пример. `specs/domain/feature-component-map.yaml` не имеет owner'а в матрице — нужно добавить.
- **Сокращённые vs полные имена компонентов** — потенциальная путаница. Либо унифицировать (один convention), либо документировать mapping (aliases в комментариях, как сделано в T-AUDIT-001 fix).
- **Status-полю нужен FSM**: `planned → in-progress-impl → covered-docs → covered-code → covered-runtime → release-ready`. Сейчас стейты декларативные.

## Related

- AUDIT-001 T-AUDIT-001, T-AUDIT-006, T-AUDIT-008, T-AUDIT-009.
- F-15 (Backtest Replay).
- [ingest-and-agents-integration.md §5 "Матрица владения"](../ingest-and-agents-integration.md).
- [feature-development-process.md §7.4.1 "Определение covered"](../feature-development-process.md).

## Open follow-ups

- **Скан остальных features**: T-AUDIT-001 закрыл только F-15. F-12 в `feature-component-map.yaml` тоже `not-implemented`, но фактически реализован (PR-F12-3a..15). Аналогично F-04 имеет `cpp/matching/src/domain/solver.hpp` в codePaths — а реально код в `solver_impl.cpp`.
- Добавить `owner` поле в `feature-component-map.yaml` записях — кто отвечает за актуализацию.
