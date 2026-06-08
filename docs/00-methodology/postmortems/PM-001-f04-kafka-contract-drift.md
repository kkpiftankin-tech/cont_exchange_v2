# PM-001 — F-04 `fills` topic Kafka contract drift

> **Type**: process post-mortem.
>
> **Status**: `published` (fix tracked by [AUDIT-001 T-AUDIT-002](../audits/AUDIT-001-feature-development-process.md), Phase 2 — after F-09 lands).
>
> **Discovered**: 2026-06-08 by external review (see `incoming-docs/2026-06-XX-Проблемы-репозитория-v1.md`).
>
> **Severity**: medium — runtime works, но контракт документации/инфры/кода рассинхронизирован, что ловит downstream consumers.

## Symptoms

Kafka топик `fills` существует в трёх местах с разными статусами:

| Источник | Что говорит |
|---|---|
| [`infra/kafka/create_topics.sh`](../../../infra/kafka/create_topics.sh) | `create_topic fills "$(retention_for_topic fills 604800000)"` — реально создаёт |
| [`docs/02-system/features/F-04-batch-clearing/feature.yaml`](../../02-system/features/F-04-batch-clearing/feature.yaml) | Упоминает `fills` в kafkaTopics (status: existing) |
| [`docs/06-api/messaging/topics.md`](../../06-api/messaging/topics.md) | `(planned, см. Conflict Note C-1 ниже) fills.md` — документация-контракт **отсутствует** |
| [`docs/06-api/messaging/fills.md`](../../06-api/messaging/) | Файл **не существует** |
| [`cpp/matching/src/infra/kafka/batch_outputs_producer.cpp`](../../../cpp/matching/src/infra/kafka/batch_outputs_producer.cpp) | `producer_.produce("fills", key, cex::common::to_bytes(fill))` — реально пишет |

Нарушено правило [CLAUDE.md §0a "Contract rule"](../../../CLAUDE.md): *"Missing contracts become `TODO contract` files, not unlinked names."* — `fills` не имеет даже TODO-stub'а.

## Root cause

1. **Не существовало кросс-валидатора Kafka контрактов**. `traceability-checker` проверяет только существование путей в `feature.yaml` / `feature-component-map.yaml`. Никто не сверяет:
   - topic в feature.yaml → строка в create_topics.sh;
   - topic в create_topics.sh → файл в docs/06-api/messaging/;
   - producer code `produce("X")` literal → docs/06-api/messaging/X.md.
2. **Conflict Note C-1 в `topics.md` помечен `(planned)` без deadline и без owner'а** — типичный пример "TODO без следующего шага" (нарушение CLAUDE.md §0a).
3. **F-04 acceptance criteria исходили из feature.yaml** (где fills упомянут как существующий), а не из реального состояния docs/code — то есть AC закрылся "зелёным" по неверным данным.

## Fix

| Action | Owner task |
|---|---|
| Создать [`docs/06-api/messaging/fills.md`](../../06-api/messaging/) | [AUDIT-001 T-AUDIT-002](../audits/AUDIT-001-feature-development-process.md) |
| Resolve Conflict Note C-1 в `topics.md` | T-AUDIT-002 |
| Добавить kafka-contract-auditor валидатор | [T-AUDIT-007](../audits/AUDIT-001-feature-development-process.md) |
| Подключить kafka-check в CI gate | [T-AUDIT-008](../audits/AUDIT-001-feature-development-process.md) (Phase 2) |
| Sync с F-09 LegFill schema extension (parentOrderId, executionGroupId, legId) | T-AUDIT-002 |

## Lessons learned

- **TODO без owner'а и без deadline — это будущий drift.** Если фрагмент классифицирован как `CONTRACT_HINT` (см. ingest-docs SKILL §Classify), он должен породить либо файл-stub `docs/06-api/.../X.md`, либо явный `TODO contract` файл с owner и due date.
- **Cross-source consistency требует кросс-валидатора.** "Каждый источник правилен в изоляции" ≠ "система согласована".
- **Acceptance criteria должны проверяться против реального state, а не against feature.yaml**. AC F-04 закрывала пункт "fills topic produced" по тексту feature.yaml — но не по присутствию `docs/06-api/messaging/fills.md` и не по running test'у.

## Related

- AUDIT-001 T-AUDIT-002, T-AUDIT-007, T-AUDIT-008.
- F-04 (Batch Clearing), F-09 (Batch / Combo Orders — extends fills schema).
- [CLAUDE.md §0a "Contract rule"](../../../CLAUDE.md).
- [docs/06-api/messaging/topics.md "Conflict Note C-1"](../../06-api/messaging/topics.md).

## Open follow-ups

- F-09 расширяет `fills` schema с `parentOrderId`/`executionGroupId`/`legId`. T-AUDIT-002 должна координироваться с F-09 PR mergе — иначе fills.md придётся переделывать.
- После kafka-contract-auditor (T-AUDIT-007) пройтись по всем остальным топикам — потенциально найдутся ещё drift'ы (особенно `execution.reports` legacy mirror).
