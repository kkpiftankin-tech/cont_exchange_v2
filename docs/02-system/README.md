# 02-system

## Назначение

Системный уровень: что должна делать платформа, какие функции у неё есть.

## Что здесь хранится

- [system-overview.md](system-overview.md) — общая картина системы
- [actors.md](actors.md) — внутренние акторы и внешние интегрирующиеся системы
- [functional-requirements.md](functional-requirements.md) — функциональные требования
- [non-functional-requirements.md](non-functional-requirements.md) — НФТ
- [features.md](features.md) — каталог фич F-01…F-17 (краткий)
- [features/](features/) — детальные описания по каждой F-XX
- [use-cases/](use-cases/) — use cases UC-F01-01…UC-F17-01

Диаграмма контекста (C4 L1) живёт в [../03-architecture/context-diagram.md](../03-architecture/context-diagram.md).

Acceptance-критерии — per-feature: блок `acceptance:` в `feature.yaml` каждой фичи и блоки `Acceptance Criteria (IN-XXX)` в [features/](features/); агрегированный план в [../10-testing/](../10-testing/).

## Как читать

QA / Product: features.md → конкретная фича → use-cases.
Архитектор: functional-requirements.md → non-functional-requirements.md → 03-architecture/.

## Связанные разделы

- [../01-business/](../01-business/) — зачем
- [../05-components/](../05-components/) — какими компонентами реализуется
- [../06-api/](../06-api/) — контракты

## Статус

draft

## Ответственные

core-team
