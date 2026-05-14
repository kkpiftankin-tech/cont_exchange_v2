# ADR-0003 — Traceability gate: каждое изменение кода связано с фичей

- **Status:** Accepted
- **Date:** 2026-05-13
- **Deciders:** core-team

## Контекст

Без явной связи между кодом и бизнес-фичей через 6 месяцев невозможно ответить на вопросы: "Зачем этот участок есть?", "Какую функциональность мы сломаем, если удалим?".

## Решение

1. **PR должен ссылаться на feature ID** в описании (например, `F-04`).
2. **`specs/domain/traceability.yaml`** связывает каждую feature → список `codePaths`.
3. **CI gate `traceability-gate.yml`** проверяет:
   - изменённый файл в `cpp/**` упомянут хотя бы в одном `feature.yaml`;
   - изменённый файл в `contracts/proto/**` упомянут в `proto-map.yaml`;
   - изменения в `infra/**` упомянуты в `docs/components/infra/component.yaml` или `feature.yaml`.
4. **Допускается waiver** через коммент в PR `traceability: waived (reason: ...)` + approval ревьюера с правами `traceability-override`.

## Альтернативы

- **Auto-generated link via git blame.** Не работает: rename / refactor разрывает связь.
- **Только labels в PR.** Не работает: легко забыть.

## Последствия

- + Любое изменение в production-коде остаётся прослеживаемым к бизнес-обоснованию
- + Удаление кода требует явного основания (фича deprecated / закрыта)
- − Лишний шум для мелких чисто-инфраструктурных PR (отсюда waiver)
