<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F18-02. Вывести чистую позицию CE из Δпозиции клиринга

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-18-ce-capital-position-hedge](../../features/F-18-ce-capital-position-hedge/) |
| ☁️ L0 system sequence | [SEQ-UC-F18-02-system](sequences/SEQ-UC-F18-02-system.md) |
| 🌊 L1 service sequence | [SEQ-F18-UC-F18-02-services](../../../05-components/sequences/SEQ-F18-UC-F18-02-services.md) |
| 💻 Source code | [`cpp/matching/src/app/ce_position_projector.cpp`](../../../../cpp/matching/src/app/) |

## Feature

- [F-18. CE Capital, Net Position & Position-Based Hedge](../../features/F-18-ce-capital-position-hedge/)

## Primary Actor

System (CE Treasury внутри Matching Backend, после каждого converged батча F-05A).

## Supporting Actors

- Matching FOB Core — источник вектора клиринга `x` и `clear_prices` (w=log-prices).
- PostgreSQL `ce_position` — persistence.
- ClickHouse `ce_position_history` — аудит.

## Preconditions

- Закрыт цикл vector clearing (F-05A / UC-F05A-02), батч **converged**
  (residual → 0). Для degraded батча Δpos не применяется (см. Alt A1).
- Каждый двусторонний сегмент имеет `venue`, `pair=BASE/QUOTE`, знаковый `x_i`,
  `P_eff` (из `w[quote]`, ADR-052/053).

## Trigger

Matching завершил `VectorClearingUseCase` и получил `converged=true` outcome.

## Main Flow

1. CE Treasury получает outcome клиринга: сегменты `seg_i`, потоки `x_i`, `w` (log-prices).
2. Для каждого сегмента `ce_position_projector` вычисляет вклад в активы:
   - `Δpos_BASE += x_i`
   - `Δpos_QUOTE += −x_i · P_eff`, где `P_eff = exp(w[quote] − w[base])`.
3. Проектор агрегирует `Δpos_a` по всем сегментам (встречные ноги неттятся).
4. CE Treasury применяет `pos_a += Δpos_a` upsert в `ce_position(asset, net_qty,
   avg_price, last_batch_id)`. Идемпотентно: если `last_batch_id == batch_id`
   уже применён — no-op.
5. Append строка в ClickHouse `ce_position_history(batch_id, asset, delta, net_after)`.
6. При `CE_NET_HEDGE_ENABLED=1` передаёт обновлённый `pos` в UC-F18-03 (хедж).

## Alternative Flows

- **A1. Degraded батч.** `converged=false` → Δpos НЕ применяется (клиринг
  ненадёжен); лог `ce_position skip: batch degraded`. Хедж не эмиттится.
- **A2. Повтор batch_id.** `last_batch_id == batch_id` → идемпотентный no-op.

## Postconditions

- `ce_position.net_qty` отражает накопленную чистую позицию по каждому активу.
- `ce_position_history` содержит запись Δposition для аудита.
- Встречные ноги внутри батча взаимно погашены (net, не gross).

## Acceptance Criteria

Покрывает F18-2, F18-3, F18-4, F18-9 из [feature.yaml](../../features/F-18-ce-capital-position-hedge/feature.yaml).

## Related Sequence Diagrams

- [SEQ-UC-F18-02-system](sequences/SEQ-UC-F18-02-system.md)
- [SEQ-F18-UC-F18-02-services](../../../05-components/sequences/SEQ-F18-UC-F18-02-services.md)

## Related Contracts

- Kafka: [batch.outputs](../../../06-api/messaging/topics.md)
- Proto: `fob.treasury.v1.CePositionDelta`, `fob.treasury.v1.CePosition`
- gRPC: `fob.treasury.v1.TreasuryService.GetCePosition`

## Related Components

- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)

## Related Data

- PostgreSQL: [ce_position](../../../07-data/ce-position.md)
- ClickHouse: `ce_position_history`

## Source Fragments

- ADR-054 §2 «Чистая позиция CE», §3 «Δпозиции из клиринга (проекция x на активы)»
