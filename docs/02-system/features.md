---
id: DOC-SYS-FEATURES
phase: 02-system
status: draft
owner: core-team
related:
  - docs/02-system/features/
  - specs/domain/feature-component-map.yaml
---

# Features Catalog

Сводный каталог фич F-01…F-17. Подробные описания — в `features/F-XX-*/`.

| ID | Name | Status | Detail |
| --- | --- | --- | --- |
| F-01 | Registration & Authentication | not-implemented | [features/F-01-auth-and-identity/](features/F-01-auth-and-identity/) |
| F-02 | Create FlowOrder | in-progress-mvp | [features/F-02-create-floworder/](features/F-02-create-floworder/) |
| F-03 | FlowOrder Lifecycle (Amend/Cancel) | in-progress-mvp | [features/F-03-order-lifecycle/](features/F-03-order-lifecycle/) |
| F-04 | Batch Clearing | in-progress-mvp | [features/F-04-batch-clearing/](features/F-04-batch-clearing/) |
| F-05 | Live Market Data | in-progress-mvp | [features/F-05-live-market-data/](features/F-05-live-market-data/) |
| F-06 | Positions / PnL / Margin | in-progress-mvp | [features/F-06-positions-pnl-margin/](features/F-06-positions-pnl-margin/) |
| F-07 | Pre-trade Risk Control | in-progress-mvp | [features/F-07-pretrade-risk/](features/F-07-pretrade-risk/) |
| F-08 | Post-trade Risk & Liquidations | not-implemented | [features/F-08-posttrade-risk-and-liquidations/](features/F-08-posttrade-risk-and-liquidations/) |
| F-09 | Batch and Combo Orders | not-implemented | [features/F-09-batch-combo-orders/](features/F-09-batch-combo-orders/) |
| F-10 | Market-Maker Liquidity Curves | not-implemented | [features/F-10-mm-curves/](features/F-10-mm-curves/) |
| F-11 | External Venues LOB → FOB | not-implemented | [features/F-11-external-venues-lob-to-fob/](features/F-11-external-venues-lob-to-fob/) |
| F-12 | Execution Hedge | not-implemented | [features/F-12-execution-hedge/](features/F-12-execution-hedge/) |
| F-13 | Post-Trade Report | not-implemented | [features/F-13-posttrade-report/](features/F-13-posttrade-report/) |
| F-14 | Deposit & Withdraw | not-implemented | [features/F-14-deposit-withdraw/](features/F-14-deposit-withdraw/) |
| F-15 | Backtest Replay | not-implemented | [features/F-15-backtest-replay/](features/F-15-backtest-replay/) |
| F-16 | Operator Console & Kill-Switch | partially-implemented | [features/F-16-operator-console/](features/F-16-operator-console/) |
| F-17 | Monitoring & Alerting | partially-implemented | [features/F-17-monitoring-and-alerts/](features/F-17-monitoring-and-alerts/) |

## Машинно-читаемые карты

- [../../specs/domain/feature-component-map.yaml](../../specs/domain/feature-component-map.yaml) — фича → компоненты, proto, code
- [../../specs/domain/traceability.yaml](../../specs/domain/traceability.yaml) — фича → файлы, топики, таблицы
- [../../specs/domain/storage-schema.yaml](../../specs/domain/storage-schema.yaml) — PG OLTP и CH OLAP схемы
