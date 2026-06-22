# PM-003 — `batch.outputs` Kafka producer/consumer schema bifurcation

> **Type**: process post-mortem.
>
> **Status**: `published` (fix tracked by [AUDIT-001 T-AUDIT-003](../audits/AUDIT-001-feature-development-process.md), Phase 2 — after F-09 lands).
>
> **Discovered**: 2026-06-08 by external review.
>
> **Severity**: medium-high — работает только за счёт defensive dual-parse в consumer; новый naive consumer сломается на текущем wire-формате.

## Symptoms

Producer и consumer используют разные proto типы для одного и того же топика `batch.outputs`.

**Producer** ([cpp/matching/src/infra/kafka/batch_outputs_producer.cpp](../../../cpp/matching/src/infra/kafka/batch_outputs_producer.cpp)):

```cpp
fob::matching::v1::BatchOutputs out;
out.mutable_result()->CopyFrom(batch);
for (auto& i : domain::BatchResultToFillEvents(batch)) {
  out.add_fills()->CopyFrom(i);
}
producer_.produce("batch.outputs", batch.batch_id(), cex::common::to_bytes(out));
```

Отправляет `BatchOutputs` — wrapper, содержащий `result` (BatchResult) + `fills` (повторяемые FillEvent).

**Consumer** ([cpp/ledger/src/infra/kafka_consumers.cpp](../../../cpp/ledger/src/infra/kafka_consumers.cpp)):

```cpp
fob::matching::v1::BatchResult batch;
fob::matching::v1::BatchOutputs out;
if (cex::common::from_bytes(payload, out)) {
  batch = out.result();
} else if (!cex::common::from_bytes(payload, batch)) {
  log_json("ERROR", "Failed to parse batch.outputs payload as BatchOutputs/BatchResult");
  return;
}
```

Дефенсивный dual-parse: сначала пробует `BatchOutputs`, fallback на raw `BatchResult`. Работает потому что producer всегда пишет первый формат, но второй путь оставлен "на всякий случай".

Документация [docs/06-api/messaging/batch-outputs.md](../../06-api/messaging/batch-outputs.md) описывает топик не полностью однозначно (нужна верификация).

## Root cause

1. **ADR на изменение wire-формата не создавался.** Когда producer был обновлён публиковать wrapper, не было обсуждения, не зафиксировано в ADR, consumer'ы не нотифицированы — просто добавили fallback "на всякий случай".
2. **Defensive coding в consumer'е скрывает контрактный долг.** Dual-parse работает, но создаёт ложное ощущение, что контракт стабилен. Новый consumer (например, F-15 backtest_uc) может написать наивный `from_bytes(payload, BatchResult)` и тихо потерять `fills` поле.
3. **Нет кросс-валидатора protobuf wire-types.** kafka-contract-auditor (T-AUDIT-007) когда появится, должен проверять что `producer.produce(topic, type)` и `consumer.subscribe(topic)+from_bytes(payload, type)` используют ОДИН type.

## Fix

Опции (выбор — в ADR-034):

**Option A** (рекомендуется в [AUDIT-001 T-AUDIT-003](../audits/AUDIT-001-feature-development-process.md)): `batch.outputs = BatchResult`. Fills уже идут в отдельный топик `fills` (см. PM-001). Wrapper `BatchOutputs` deprecated → удалить proto, упростить producer/consumer.

**Option B**: оставить `batch.outputs = BatchOutputs`. Удалить fallback path в consumer. Все consumers ОБЯЗАНЫ ожидать wrapper. fills topic остаётся параллельным каналом.

| Action | Owner task |
|---|---|
| Создать ADR-034 с выбором Option A | T-AUDIT-003 |
| Применить выбранный вариант в [`batch_outputs_producer.cpp`](../../../cpp/matching/src/infra/kafka/batch_outputs_producer.cpp) и всех consumers (ledger, market_data, observability, backtest) | T-AUDIT-003 |
| kafka-contract-auditor проверка producer-type ≡ consumer-type | T-AUDIT-007 |
| Документировать canonical schema в [docs/06-api/messaging/batch-outputs.md](../../06-api/messaging/batch-outputs.md) | T-AUDIT-003 |

**Координация с F-09**: F-09 эмиттит `ExecutionGroup` в отдельный топик `execution.groups`, поэтому не влияет напрямую на `batch.outputs`. Но изменения в matching producer code могут вызвать merge conflicts с F-09 PR — поэтому Phase 2.

## Lessons learned

- **Любое изменение wire-формата Kafka топика требует ADR**, даже если "просто обернуть в wrapper". ADR форсирует discovery всех downstream consumers.
- **Defensive dual-parse — это accept'ор технического долга, не решение.** В долгосрочной перспективе он усложняет миграцию.
- **proto message type должен быть зафиксирован в `docs/06-api/messaging/<topic>.md`** как обязательное поле schema-секции (не "схема"  словом, а конкретный `package.message` идентификатор).

## Related

- AUDIT-001 T-AUDIT-003, T-AUDIT-007.
- PM-001 (F-04 fills topic) — связанная проблема в том же подсемействе батчевых топиков.
- F-04 (Batch Clearing), F-09 (Combo Orders, expanded fills), F-15 (Backtest replay).
- ADR-034 (планируется).

## Open follow-ups

- Проверить, не используют ли `market_data`, `observability` consumers ту же схему — может быть нужна общая координация.
- Backtest replay в F-15 читает `batch.outputs` для parity-checks — после T-AUDIT-003 убедиться что путь backtest не сломан.
