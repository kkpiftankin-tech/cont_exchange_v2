<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F06-01. Показать позиции / PnL / маржу

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-06-positions-pnl-margin](../../features/F-06-positions-pnl-margin/) |
| ☁️ L0 system sequence | [SEQ-UC-F06-01-system](sequences/SEQ-UC-F06-01-system.md) — system как чёрный ящик |
| 🌊 L1 service sequence | [SEQ-F06-UC-F06-01-services](../../../05-components/sequences/SEQ-F06-UC-F06-01-services.md) — взаимодействие сервисов |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-06. Positions / PnL / Margin](../../features/F-06-positions-pnl-margin/)

## Primary Actor

Trader

## Preconditions

- Пользователь аутентифицирован.

## Trigger

Trader запрашивает текущие позиции (UI dashboard или API).

## Main Flow

1. Trader посылает `GetPositions(user_id)` через gateway.
2. Gateway проксирует в Ledger / Risk.
3. Ledger возвращает балансы и позиции.
4. Risk возвращает текущую маржу, PnL, leverage.
5. UI отображает.

## Postconditions

- Trader видит актуальное состояние без изменения данных.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F06-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F06-UC-F06-01-services.md)

## Related Contracts

- `fob.ledger.v1.LedgerService` — `GetBalances`, `GetPositions`
- `fob.risk.v1.RiskService` — `GetRiskSnapshot`

## Related Components

- [gateway](../../../05-components/gateway/overview.md)
- [ledger](../../../05-components/ledger/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)

## Related Data

- [accounts, positions, risk_snapshots](../../../07-data/data-overview.md)
