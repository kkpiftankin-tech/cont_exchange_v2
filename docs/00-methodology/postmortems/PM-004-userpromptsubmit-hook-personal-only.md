# PM-004 — `UserPromptSubmit` auto-archive hook not registered in shared settings

> **Type**: process post-mortem.
>
> **Status**: `published` (fix landed via [AUDIT-001 T-AUDIT-004](../audits/AUDIT-001-feature-development-process.md), 2026-06-08).
>
> **Discovered**: 2026-06-08 by external review.
>
> **Severity**: medium-high — central documentation (CLAUDE.md, feature-development-process.md) обещала автоматическое архивирование вложений, реальность отличалась от обещания, новые разработчики получали ломаный pipeline.

## Symptoms

[CLAUDE.md §0a "Auto-archive of chat attachments"](../../../CLAUDE.md) гласит:

> A `UserPromptSubmit` hook ([tools/auto-archive-attachments.py](../../../tools/auto-archive-attachments.py)) парсит `<document>` блоки в каждом пользовательском промте и сохраняет их в `incoming-docs/YYYY-MM-DD-<slug>-<sha8>.md` **до** того, как ассистент увидит сообщение.

Реальность на ветке `feature/F-15-ephemeral-persist` (и далее по большинству feature-branches):

- [`.claude/settings.json`](../../../.claude/settings.json) (shared, committed) — содержит только `PostToolUse` hook, **никакого `UserPromptSubmit`**.
- `.claude/settings.local.json` (per-user, gitignored) — у текущего пользователя содержит только `permissions.allow` блок, тоже **без `UserPromptSubmit`**.
- Сам скрипт [`tools/auto-archive-attachments.py`](../../../tools/auto-archive-attachments.py) существует, но **никем не вызывается** автоматически.

Результат: на свежем clone репозитория, на CI, у нового разработчика — `<document>` блоки в чатах не архивируются. Pipeline ingest сломан до того, как ingest-docs skill запустится. Документация лжёт.

## Root cause

1. **Hook был изначально зарегистрирован в `.claude/settings.local.json`** (per-user, gitignored), потом этот файл был перезаписан пользовательскими `permissions.allow` правилами и hook затёрся. Никто не заметил, потому что у того, кто перезаписывал, файл копировался без блока hook.
2. **Документация описывала желаемое поведение, не текущее.** [CLAUDE.md §0a](../../../CLAUDE.md) и [feature-development-process.md §1.1](../feature-development-process.md) обещали, что hook работает "из коробки" — но это утверждение никогда не верифицировалось.
3. **Не было smoke-test'а на наличие/работу hook'а в CI.** Любая регрессия не ловилась.

## Fix

| Action | Owner task / commit |
|---|---|
| Перенести `UserPromptSubmit` hook в `.claude/settings.json` (shared, committed) | [AUDIT-001 T-AUDIT-004](../audits/AUDIT-001-feature-development-process.md), 2026-06-08 |
| Добавить `--self-test` режим в [`tools/auto-archive-attachments.py`](../../../tools/auto-archive-attachments.py) — smoke-проверка regex parser без I/O | T-AUDIT-004 |
| Добавить `make hooks-check` target в [Makefile](../../../Makefile) | [T-AUDIT-009](../audits/AUDIT-001-feature-development-process.md), 2026-06-08 |
| Обновить CLAUDE.md §0a — указать точную локацию (shared settings.json) + smoke-test команду | T-AUDIT-004 |
| Создать [`.claude/settings.example.json`](../../../.claude/settings.example.json) — пример для personal overrides | [T-AUDIT-005](../audits/AUDIT-001-feature-development-process.md), 2026-06-08 |
| Подключить `make hooks-check` в CI (governance.yml) | [T-AUDIT-008](../audits/AUDIT-001-feature-development-process.md) (Phase 2) |

## Lessons learned

- **Если документ обещает поведение — оно должно быть verifiable**. Smoke-test'ы для каждого упомянутого hook'а или automation — обязательны.
- **`.claude/settings.local.json` нельзя использовать для shared behavior.** gitignored ⇒ невоспроизводимо ⇒ docs лгут на любой свежей машине.
- **Hooks нужно прогонять в CI.** Один `--self-test` flag + одна строка в workflow — и регрессия ловится за минуты, а не за месяцы.
- **README/Development.md должен содержать "Setting up Claude Code"** раздел для новых разработчиков с конкретными командами. Сделано в T-AUDIT-005.

## Related

- AUDIT-001 T-AUDIT-004, T-AUDIT-005, T-AUDIT-008, T-AUDIT-009.
- [CLAUDE.md §0a](../../../CLAUDE.md) — обновлён, теперь корректно описывает реальность.
- [feature-development-process.md §1.1](../feature-development-process.md) — обновлён.
- [README §8 "Claude Code settings"](../../../README.md) — добавлен setup раздел.

## Open follow-ups

- Аудитировать остальные claim'ы в [CLAUDE.md](../../../CLAUDE.md) — есть ли ещё обещания, которые не verify'ятся в CI?
- Рассмотреть добавление check'а "settings.json contains expected hooks" в `make hooks-check` (сейчас только smoke-test самого парсера).
