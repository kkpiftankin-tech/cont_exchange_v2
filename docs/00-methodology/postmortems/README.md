# Process Post-Mortems

Этот каталог содержит **process post-mortems** — разборы причин, по которым процесс
разработки (а не сервис в проде) позволил drift'у/ошибке появиться. Не путать с
service incident post-mortems (для runtime инцидентов — отдельная папка под
[`docs/11-operations/`](../../11-operations/) когда понадобится).

## Цель

Превращать ошибки процесса в **новые validators / gates / документационные правила**,
чтобы тот же класс drift'а не повторился молча.

## Шаблон

Все PM пишутся по [`_template.md`](./_template.md). Обязательные секции:
Symptoms → Root cause → Fix → Lessons learned → Related.

## Жизненный цикл

1. **draft** — создан, но fix ещё не разработан.
2. **published** — fix landed (commit / PR ссылки в Fix section).
3. **closed** — fix verified в проде (если применимо) + open follow-ups закрыты.

## Текущие записи

| ID | Title | Status | Owner audit |
|---|---|---|---|
| [PM-001](PM-001-f04-kafka-contract-drift.md) | F-04 `fills` topic Kafka contract drift | published | [AUDIT-001 T-AUDIT-002](../audits/AUDIT-001-feature-development-process.md) |
| [PM-002](PM-002-f15-status-drift.md) | F-15 status drift между `feature.yaml` и `feature-component-map.yaml` | published | [AUDIT-001 T-AUDIT-001](../audits/AUDIT-001-feature-development-process.md) |
| [PM-003](PM-003-batch-outputs-schema-bifurcation.md) | `batch.outputs` producer/consumer schema bifurcation | published | [AUDIT-001 T-AUDIT-003](../audits/AUDIT-001-feature-development-process.md) |
| [PM-004](PM-004-userpromptsubmit-hook-personal-only.md) | UserPromptSubmit hook not registered in shared settings | published | [AUDIT-001 T-AUDIT-004](../audits/AUDIT-001-feature-development-process.md) |

## Как добавить новый PM

1. Скопировать `_template.md` → `PM-NNN-short-slug.md` (NNN = следующий свободный номер).
2. Заполнить секции, упомянуть AUDIT-NNN или конкретные tasks, которые fix отслеживают.
3. Добавить строку в таблицу выше.
4. Линковать из [feature-development-process.md](../feature-development-process.md) или
   [CLAUDE.md](../../../CLAUDE.md), если касается process-wide invariant.
