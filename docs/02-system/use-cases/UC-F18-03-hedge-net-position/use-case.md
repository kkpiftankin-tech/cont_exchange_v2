<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F18-03. Хеджировать чистую позицию сверх порога

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-18-ce-capital-position-hedge](../../features/F-18-ce-capital-position-hedge/) |
| ☁️ L0 system sequence | [SEQ-UC-F18-03-system](sequences/SEQ-UC-F18-03-system.md) |
| 🌊 L1 service sequence | [SEQ-F18-UC-F18-02-services](../../../05-components/sequences/SEQ-F18-UC-F18-02-services.md) |
| 💻 Source code | [`cpp/matching/src/app/ce_net_hedge_builder.cpp`](../../../../cpp/matching/src/app/) |

## Feature

- [F-18. CE Capital, Net Position & Position-Based Hedge](../../features/F-18-ce-capital-position-hedge/)

## Primary Actor

System (CE Treasury после обновления `pos` в UC-F18-02).

## Supporting Actors

- Risk Manager — `PreHedgeCheck` (переиспользование F-12).
- Execution Planning / External Venues — исполнение хеджа.
- Settlement Ledger — realized PnL → капитал (UC-F18-01).

## Preconditions

- `CE_NET_HEDGE_ENABLED=1` и `F05A_MONEY_ENABLED=1`.
- Задан `CE_HEDGE_THRESHOLD_<ASSET>` (θ_a) и `CE_HEDGE_QUOTE_REF_<ASSET>`.
- `pos_a` обновлён последним converged батчем (UC-F18-02).

## Trigger

Обновление `ce_position` в UC-F18-02.

## Main Flow

1. CE Treasury (`ce_net_hedge_builder`) для каждого актива `a` проверяет `|pos_a| > θ_a`.
2. Если да, формирует хедж-инструмент `a/QUOTE_ref` (напр. `BTC/USDT`),
   `qty = |pos_a|`, `side = SELL` если `pos_a>0`, иначе `BUY` (flatten к нулю).
3. Вызывает `RiskService.PreHedgeCheck` (maxNotional, exposure, slippage).
4. При ACCEPT публикует **один** `ExecutionIntent` на актив в Kafka
   `execution.intents` (key = `ce|hedge|<asset>`). Заменяет per-segment
   `BuildHedgeIntents` (тот за legacy-флагом).
5. External Venues исполняют, возвращают `ExecutionReport` (Kafka `execution.venue`).
6. Settlement Ledger применяет realized PnL/fees → `ce_capital` (UC-F18-01),
   уменьшает `pos_a` на исполненный объём.

## Alternative Flows

- **A1. Под порогом.** `|pos_a| <= θ_a` → хедж не эмиттится (накапливается до
  следующего батча).
- **A2. Встречный неттинг.** `Δpos_a ≈ 0` после неттинга (UC-F18-02) → позиция не
  выросла → хедж не нужен.
- **A3. Risk REJECT.** `PreHedgeCheck` REJECT → intent не публикуется; structured
  log + (follow-up) risk.alert.
- **A4. Legacy режим.** `CE_NET_HEDGE_ENABLED=0` → per-segment `BuildHedgeIntents`
  (ADR-049), F-18 путь неактивен.

## Postconditions

- Для каждого актива сверх порога опубликован ровно один net-хедж intent (flatten).
- После исполнения `pos_a` уменьшена, `ce_capital` обновлён realized PnL/fees.
- Хедж-объём ≤ per-segment gross (за счёт неттинга).

## Acceptance Criteria

Покрывает F18-5, F18-6, F18-7, F18-9 из [feature.yaml](../../features/F-18-ce-capital-position-hedge/feature.yaml).

## Related Sequence Diagrams

- [SEQ-UC-F18-03-system](sequences/SEQ-UC-F18-03-system.md)
- [SEQ-F18-UC-F18-02-services](../../../05-components/sequences/SEQ-F18-UC-F18-02-services.md)

## Related Contracts

- Kafka: [execution.intents](../../../06-api/messaging/execution-intents.md), [execution.venue](../../../06-api/messaging/execution-venue.md)
- gRPC: [risk-pre-hedge-check](../../../06-api/grpc/risk-pre-hedge-check.md)
- Proto: `fob.treasury.v1.CeNetHedgeIntent`, `fob.execution.v1.ExecutionIntent`

## Related Components

- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [external-venues](../../../05-components/external-venues/overview.md)
- [ledger](../../../05-components/ledger/overview.md)

## Related Data

- PostgreSQL: [ce_position](../../../07-data/ce-position.md), [ce_capital](../../../07-data/ce-capital.md)

## Source Fragments

- ADR-054 §4 «Хедж от net-позиции (не per-segment)», §5 «Обновление капитала»
