# ADR-0004 — Policy для docs-code drift

- **Status:** Accepted
- **Date:** 2026-05-13
- **Deciders:** core-team

## Контекст

Drift = расхождение между документацией/спецификациями и реальным кодом. Накапливается со временем, если не контролируется автоматически.

## Решение

Drift детектируется в CI скриптом `tools/docs-code-drift/check.py`. Поведение:

| Тип изменения | Что требуется |
|---|---|
| Изменён файл в `cpp/**` | Изменения в `docs/components/<id>/` или `docs/features/F-XX/` |
| Изменён файл в `contracts/proto/**` | Изменения в `specs/contracts/proto-map.yaml` |
| Изменены Kafka топики (имя/retention) | Обновление `docs/architecture/event-driven-architecture.md` и `docs/components/infra/component.yaml` |
| Изменены env-переменные | Обновление `docs/components/<id>/component.yaml.config` |
| Удалена функция/класс | Обновление `feature.yaml.codePaths` (удаление пути) + проверка `knownIssues` |

**Drift = блокирующая ошибка PR.** Waiver возможен через явную аппрувку:

```
# в PR description:
drift-waiver: feature-doc-pending-PR-#123
```

## Что НЕ блокирует

- Изменения в `cpp/common/src/log.cpp` (форматирование строк лога) — не меняет API.
- Изменения в `*.cpp` без правки `.hpp` и без изменения публичного поведения — кандидат на waiver.
- Чисто-CMake / форматирование / комментарии — не блокирует, но `git diff --stat > 100 строк` без doc-обновления получает warning.

## Альтернативы

- **Только soft warning.** Отклонено: накапливается, никогда не чинится.
- **Doc-first: запрет реализации до approval доков.** Отклонено: затрудняет хотfixы.

## Последствия

- + Документация и код синхронны
- + Меньше "архаичных" блоков кода без объяснения
- − При большом рефакторе нужен большой doc-PR
