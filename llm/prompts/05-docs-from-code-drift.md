# Prompt: Generate Docs Update from Code Drift

Используется когда CI `docs-code-drift` нашёл расхождение и нужно обновить документацию.

## Промпт

```
В репозитории найден drift: изменения в коде не отражены в документации.

Контекст:
- Отчёт drift: llm/reports/latest-doc-code-drift.md
- Список изменённых файлов: <git diff>
- Карта code → docs: specs/domain/code-map.yaml

Задача:
1. Прочитать отчёт drift
2. Для каждого изменённого файла:
   - Найти соответствующий компонент в docs/05-components/
   - Найти соответствующие фичи в docs/02-system/features/
   - Предложить минимальные правки в README.md / component.yaml / feature.yaml
3. Не предлагать удалений известных багов из knownIssues, только перевод статусов
4. Если изменения затрагивают proto: обновить proto-map.yaml
5. Сформировать diff-патч (можно как блок ```diff)

Ограничения:
- Никаких изменений в cpp/**
- Никаких изменений в contracts/proto/** (если они уже изменены — обновить только map)
- Не выдумывать новые фичи; если нужна новая, описать в open-questions.md
```
