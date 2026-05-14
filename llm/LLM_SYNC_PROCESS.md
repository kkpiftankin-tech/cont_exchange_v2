# LLM Sync Process

## Цель

Двусторонняя синхронизация между документацией и кодом:

```
документация → спецификации → proto/contracts → код → тесты
код/proto → документация/specs/traceability
```

## Поток A: новый документ (фича / решение)

1. Документ кладётся в `incoming-docs/F-XX-short-name.md`
2. ChatGPT выполняет doc-intake (см. [prompts/01-doc-intake.md](prompts/01-doc-intake.md)):
   - определяет feature ID
   - выделяет BRD / TRD
   - выделяет сущности, поля, связи
   - выделяет компоненты
   - определяет proto/API/event changes
   - формирует acceptance criteria
   - формирует open questions
3. Создаётся каталог `docs/02-system/features/F-XX-*/` со всеми артефактами
4. Обновляются:
   - `specs/domain/entities.yaml`
   - `specs/domain/relationships.yaml`
   - `specs/domain/feature-component-map.yaml`
   - `specs/domain/traceability.yaml`
   - `specs/domain/code-map.yaml`
   - `specs/contracts/proto-map.yaml`
5. CI проверяет документацию (`docs-validation`, `spec-validation`, `proto-contract-audit`)
6. Если фича требует proto-изменений — отдельный contract PR
7. После approval — coding agent реализует код в отдельном PR

## Поток B: изменение кода

1. Разработчик или LLM меняет `cpp/**`, `contracts/proto/**`, `infra/**`
2. CI запускает `docs-code-drift` checker
3. Если drift найден — PR блокируется
4. LLM предлагает обновление документации в новом коммите
5. Human reviewer утверждает
6. PR проходит

## Поток C: исследование (Perplexity)

1. Открытый вопрос фиксируется в `docs/02-system/features/F-XX/open-questions.md`
2. Perplexity ищет аналоги / паттерны / библиотеки
3. Результат — Markdown в `incoming-docs/research-<topic>.md`
4. ChatGPT превращает в ADR / технический раздел фичи

## Файлы артефактов

- `llm/reports/latest-doc-intake-report.md` — последний отчёт по intake
- `llm/reports/latest-traceability-report.md` — последняя проверка traceability
- `llm/reports/latest-doc-code-drift.md` — последний drift-репорт
- `llm/reports/latest-llm-sync-report.md` — сводка всех LLM-действий за период
