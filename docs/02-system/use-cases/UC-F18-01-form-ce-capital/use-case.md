<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F18-01. Сформировать капитал CE (seed + PnL/fees)

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-18-ce-capital-position-hedge](../../features/F-18-ce-capital-position-hedge/) |
| ☁️ L0 system sequence | [SEQ-UC-F18-01-system](sequences/SEQ-UC-F18-01-system.md) |
| 🌊 L1 service sequence | [SEQ-F18-UC-F18-02-services](../../../05-components/sequences/SEQ-F18-UC-F18-02-services.md) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-18. CE Capital, Net Position & Position-Based Hedge](../../features/F-18-ce-capital-position-hedge/)

## Primary Actor

System (CE Treasury при старте matching-сервиса) + Operator (перенастройка seed).

## Supporting Actors

- Settlement Ledger — источник realized PnL/fees.
- PostgreSQL `ce_capital` — persistence.

## Preconditions

- Заданы env `CE_CAPITAL_SEED_<ASSET>` (напр. `CE_CAPITAL_SEED_BTC=10`,
  `CE_CAPITAL_SEED_USDT=1000000`).
- Таблица `ce_capital` существует (init.sql).

## Trigger

Старт CE Treasury (внутри matching) **или** приход `ExecutionReport` хеджа с
realized PnL/fees.

## Main Flow

1. При старте CE Treasury читает `CE_CAPITAL_SEED_<ASSET>` для всех активов.
2. Для каждого актива upsert `ce_capital(asset, seed_amount, balance=seed_amount,
   realized_pnl=0)` — идемпотентно (не перезатирает уже накопленный balance при
   рестарте, если `ce_capital` уже seeded; seed применяется только при пустой строке).
3. При приходе `ExecutionReport` (Kafka `execution.venue`): Settlement Ledger
   считает realized PnL/fees хеджа (формула F-12 DoD-6).
4. CE Treasury применяет дельту: `ce_capital.balance += pnl_delta − fee`,
   `ce_capital.realized_pnl += pnl_delta`. Идемпотентно по `report_id`.
5. Снапшот капитала доступен через `TreasuryService.GetCeCapital` (UI).

## Postconditions

- `ce_capital` содержит по одной строке на актив с актуальными `balance`,
  `realized_pnl`.
- Повторное применение того же `report_id` не меняет капитал (idempotent).

## Acceptance Criteria

Покрывает F18-1, F18-7 из [feature.yaml](../../features/F-18-ce-capital-position-hedge/feature.yaml).

## Related Sequence Diagrams

- [SEQ-UC-F18-01-system](sequences/SEQ-UC-F18-01-system.md)
- [SEQ-F18-UC-F18-02-services](../../../05-components/sequences/SEQ-F18-UC-F18-02-services.md)

## Related Contracts

- gRPC: `fob.treasury.v1.TreasuryService.GetCeCapital`
- Kafka: [execution.venue](../../../06-api/messaging/execution-venue.md)
- Proto: `fob.treasury.v1.CeCapital`

## Related Data

- PostgreSQL: [ce_capital](../../../07-data/ce-capital.md)

## Source Fragments

- ADR-054 §1 «Капитал CE», §5 «Обновление капитала (realized PnL)»
