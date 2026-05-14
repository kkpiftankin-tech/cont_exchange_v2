# LLM Governance

## Принципы

1. **LLM не правит код напрямую.** Изменения в `cpp/**` и `contracts/proto/**` проходят через PR с human review.
2. **Документация ведёт код, а не наоборот.** Новые фичи всегда начинаются с документа в `incoming-docs/`, не с кода.
3. **`contracts/proto/fob/**` — единственный источник истины** для wire-формата. LLM не описывает сообщения в Markdown — только ссылается.
4. **Любое предложение LLM по изменению кода должно ссылаться на feature ID** (F-02, F-04, …) и соответствующий `feature.yaml`.
5. **CI блокирует изменения без обновления документации** ([docs-code-drift.yml](../.github/workflows/docs-code-drift.yml)).

## Роли LLM в проекте

| LLM | Назначение | Файл инструкций |
|---|---|---|
| ChatGPT (Project) | Doc-intake, нормализация фич, code review | [CHATGPT_PROJECT_INSTRUCTIONS.md](CHATGPT_PROJECT_INSTRUCTIONS.md) |
| Perplexity | Исследования (паттерны, аналоги, библиотеки) | [PERPLEXITY_RESEARCH_GUIDE.md](PERPLEXITY_RESEARCH_GUIDE.md) |
| Cloud Code / coding agent | Реализация код-изменений из feature.yaml | [CLOUDCODE_CODING_GUIDE.md](CLOUDCODE_CODING_GUIDE.md) |

## Запрещено для LLM

- Менять `cpp/**` без явно указанного feature ID в commit message
- Менять `contracts/proto/**` без обновления `specs/contracts/proto-map.yaml`
- Менять risk/ledger/matching логику без сопровождающих тестов (когда они появятся)
- Удалять требования из документации без явного approval
- Добавлять секреты / credentials в любые файлы
- Использовать `legacy_mvp` как target для новых фич (только как референс)

## Обязательные действия LLM при предложении PR

1. Перечислить изменённые файлы с указанием категории (code / docs / specs / contracts)
2. Привязать к feature ID
3. Указать, какие acceptance criteria покрываются
4. Если меняются proto: явно отметить как breaking / non-breaking
5. Если меняется state в ledger / risk / matching: предупредить о необходимости тестов
