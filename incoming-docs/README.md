# incoming-docs/

Сюда складываются **новые входящие материалы** перед обработкой и раскладкой по `docs/0X-*/`.

## Правило именования

Любой новый документ (пояснительная записка, спецификация, исследование, ТЗ от заказчика)
сначала попадает сюда — **с датой в имени файла**:

```
incoming-docs/YYYY-MM-DD-<name>.md
```

## Автоматическое сохранение аттачей из чата

Файлы, **прикреплённые к сообщению в чате Claude Code**, автоматически сохраняются в `incoming-docs/` через `UserPromptSubmit` хук:

- Скрипт: [`../tools/auto-archive-attachments.py`](../tools/auto-archive-attachments.py)
- Регистрация: [`../.claude/settings.local.json`](../.claude/settings.local.json) → `hooks.UserPromptSubmit`
- Формат имени: `YYYY-MM-DD-<slug>-<sha8>.md`, где `<sha8>` — первые 8 hex от sha1 содержимого (для dedupe re-uploads).
- Шапка каждого файла содержит метки `<!-- auto-archived ... -->` с датой и оригинальным именем source.

Хук срабатывает **до** того, как ассистент увидит сообщение, поэтому файл уже на диске к моменту обработки. После сохранения хук эмиттит system-reminder с перечнем сохранённого.

Ограничения хука: работает только с inline-`<document>` блоками; изображения и бинарные файлы не обрабатываются.

## Жизненный цикл

1. **Inbox.** Файл лежит в `incoming-docs/`.
2. **Doc intake.** Содержание разложено по `docs/01-business/…11-operations/`. На исходник остаётся ссылка из релевантных мест.
3. **Архив.** Исходный файл не удаляется — остаётся ссылочной точкой и для будущих сверок.

## Текущее содержимое

| Файл | Куда разложен |
| --- | --- |
| [EXPLANATORY-NOTE-source.md](EXPLANATORY-NOTE-source.md) | концепции → `04-domain/`, роли → `01-business/stakeholders.md`, БД → `07-data/`, фичи → `02-system/features/` |
| [2026-05-13-Этапы.md](2026-05-13-Этапы.md) | методология → `../ЭТАПЫ.md`, структура → весь `docs/` |

См. также:

- [index.md](index.md) — индекс с IN-ID и статусами
- [../.claude/skills/ingest-docs/SKILL.md](../.claude/skills/ingest-docs/SKILL.md) — pipeline ingestion
- [../docs/00-methodology/document-ingestion.md](../docs/00-methodology/document-ingestion.md) — методология ingestion
- [../ЭТАПЫ.md](../ЭТАПЫ.md) — методология docs-as-code
- [../docs/README.md](../docs/README.md) — карта документации
- [../llm/LLM_SYNC_PROCESS.md](../llm/LLM_SYNC_PROCESS.md) — процесс doc-intake
