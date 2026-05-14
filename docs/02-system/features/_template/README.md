# F-XX — Short Feature Name

> **Template.** Скопируйте каталог `_template/` в `F-XX-short-name/` и замените плейсхолдеры. Реальные феatures см. напр. [F-04 Batch Clearing](../F-04-batch-clearing/).

## Краткое описание

Что делает фича, зачем нужна.

## Артефакты фичи

После копирования каталог должен содержать:

- `README.md` (этот файл) — обзор фичи.
- `feature.yaml` — машиночитаемая мета (id, status, owner, acceptance, traceability).
- `acceptance-criteria.md` — критерии приёмки (создаётся при ингесте; источник истины — блок `acceptance:` в `feature.yaml`).
- `open-questions.md` — открытые вопросы и решения.
- `traceability.yaml` — связь с IN-XXX фрагментами и UC-XX use cases.

## Use Case(-ы)

- `../../use-cases/UC-FXX-NN-short-name/use-case.md` (UC-FXX-NN)

## Sequence Diagrams

- System-level: `docs/02-system/use-cases/UC-FXX-NN-short-name/sequences/SEQ-UC-FXX-NN-system.md`
- Service-level: `docs/05-components/sequences/SEQ-FXX-UC-FXX-NN-services.md`

## Related Components

- ссылки на `docs/05-components/<component>/overview.md`

## Related Contracts

- ссылки на `docs/06-api/{grpc,rest,messaging}/...`

## Related Data Objects

- ссылки на `docs/07-data/oltp-schema.md` / `olap-schema.md`

## Source Fragments

- IN-XXX-FR-YYY
