---
id: ADR-029
status: accepted
date: 2026-05-28
owners:
  - architecture
related:
  - docs/03-architecture/adr/ADR-006-docs-as-code-repository-structure.md
  - docs/03-architecture/adr/ADR-007-feature-traceability-gate.md
  - docs/03-architecture/adr/ADR-008-code-doc-drift-policy.md
  - CLAUDE.md
---

# ADR-029: LLM-assisted development governance

## Контекст

Проект активно использует LLM (Claude Code) для docs-ingestion, генерации
документации и branch-local правок кода. ADR-006/007/008 описывают
docs-as-code, traceability и drift, но нет отдельного ADR, регламентирующего
LLM как участника разработки. Без правил растёт риск неконтролируемых правок,
утечки секретов и docs-code drift.

## Решение

Зафиксировать governance-правила для LLM-ассистированной разработки:

- **Роли**: LLM-ассистент (Claude Code) — branch-local правки кода и
  документации; архитектурный review/спеки — через человека; внешний research
  — отдельно и только как справка.
- **Никаких прямых коммитов в `main`**: весь LLM-вывод проходит через PR и
  human review.
- **No secrets**: LLM не вносит и не запрашивает секреты в репозиторий
  (CLAUDE.md §22); секреты не попадают в Kafka/доки.
- **Docs-before-code**: LLM создаёт/обновляет документацию и
  implementation-tasks **до** кода (CLAUDE.md §0a, [ADR-018](ADR-018-design-traceability-chain.md)).
- **Drift review**: расхождение код/доки прогоняется через drift-policy
  ([ADR-008](ADR-008-code-doc-drift-policy.md)); LLM предлагает doc-update,
  финальное решение — человек.
- **Immutable incoming**: входящие документы архивируются в `incoming-docs/`
  и неизменяемы (CLAUDE.md §0a).
- **Reports/prompt-library** (рекомендация): повторно используемые промпты и
  отчёты об ingestion/impl/drift хранятся в репозитории, чтобы процесс был
  воспроизводим и аудируем.

## Альтернативы

- **Без формального governance** — отклонено: риск drift, прямых правок, утечек.
- **Запрет LLM** — отклонено: проект сознательно построен на docs-as-code + LLM-ассистировании.

## Последствия

- **Плюс:** аудируемый, воспроизводимый процесс; LLM-вывод всегда проходит human gate.
- **Минус:** накладные расходы на PR/review даже для мелких LLM-правок.

## Обратимость

Высокая. Чисто процессное решение; не затрагивает рантайм.
