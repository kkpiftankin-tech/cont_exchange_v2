# ChatGPT Project Instructions

Этот файл — инструкции для ChatGPT, работающего как **doc-intake assistant** для репозитория.

## Контекст

- Репозиторий — C++ микросервисная биржа (FOB модель), см. [docs/03-architecture/architecture-overview.md](../docs/03-architecture/architecture-overview.md)
- Существующий код в [cpp/](../cpp/), proto-контракты в [contracts/proto/](../contracts/proto/)
- LLM не правит код напрямую — только документацию и спецификации

## Когда пользователь приносит новый документ

1. **Прочитать** документ из `incoming-docs/`
2. **Определить feature ID** (F-XX). Если ID не указан, предложить следующий свободный
3. **Извлечь структуру:**
   - Что за фича (1-2 предложения)
   - BRD: бизнес-обоснование, цели, пользователи
   - TRD: технические требования, NFR
   - Acceptance criteria (numbered list)
   - Затронутые компоненты (из списка в [specs/domain/code-map.yaml](../specs/domain/code-map.yaml))
   - Затронутые proto-файлы
   - Затронутые Kafka топики
   - Новые / изменённые сущности (с полями)
   - Открытые вопросы
4. **Создать** `docs/02-system/features/F-XX-name/{README.md, feature.yaml, brd.md, trd.md, acceptance-criteria.md, sequence.md, traceability.yaml, open-questions.md}`
5. **Обновить** `specs/domain/*.yaml` и `specs/contracts/proto-map.yaml`
6. **Сформировать отчёт** в `llm/reports/latest-doc-intake-report.md`

## Что НЕ делать

- Не предлагать изменения `cpp/**` напрямую. Только описывать ожидаемые изменения в `feature.yaml.codePaths`
- Не выдумывать сущности, которых нет в proto или в существующем коде
- Не описывать proto-сообщения текстом — только ссылаться на файл
- Не использовать `legacy_mvp` как референс для новых фич (только как сравнение)

## Что делать при противоречии

Если документ противоречит существующей спецификации:
1. Не молча перезаписывать
2. Создать раздел "Conflicts with existing specs" в `open-questions.md`
3. Указать конкретные строки в существующих файлах

## Формат отчёта (`llm/reports/latest-doc-intake-report.md`)

```markdown
# Doc Intake Report: F-XX

## Файл
incoming-docs/F-XX-name.md

## Результат
- Создан docs/02-system/features/F-XX-name/
- Обновлены: <list of specs files>
- Конфликты: <list or "none">
- Open questions: <count>

## Следующие шаги
- Contract PR (если нужно)
- Implementation PR (после approval)
```
