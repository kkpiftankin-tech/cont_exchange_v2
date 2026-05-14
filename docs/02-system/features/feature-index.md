---
id: DOC-FEATURE-INDEX
phase: 02-system
status: draft
owner: core-team
---

# Feature Index

Сводный индекс продуктовых фич. Источник истины по статусам и связям — `feature.yaml`
внутри каждой папки фичи и [../../../specs/domain/feature-component-map.yaml](../../../specs/domain/feature-component-map.yaml).

| ID | Название | Use Case (primary) | System SEQ | Service SEQ |
| --- | --- | --- | --- | --- |
| [F-01](F-01-auth-and-identity/) | Authentication & Identity | [UC-F01-01](../use-cases/UC-F01-01-authenticate-user/use-case.md) | [system](../use-cases/UC-F01-01-authenticate-user/sequences/SEQ-UC-F01-01-system.md) | [services](../../05-components/sequences/SEQ-F01-UC-F01-01-services.md) |
| [F-02](F-02-create-floworder/) | Create FlowOrder | [UC-F02-01](../use-cases/UC-F02-01-create-flow-order/use-case.md) | [system](../use-cases/UC-F02-01-create-flow-order/sequences/SEQ-UC-F02-01-system.md) | [services](../../05-components/sequences/SEQ-F02-UC-F02-01-services.md) |
| [F-03](F-03-order-lifecycle/) | Amend / Cancel FlowOrder | [UC-F03-01](../use-cases/UC-F03-01-amend-cancel-order/use-case.md) | [system](../use-cases/UC-F03-01-amend-cancel-order/sequences/SEQ-UC-F03-01-system.md) | [services](../../05-components/sequences/SEQ-F03-UC-F03-01-services.md) |
| [F-04](F-04-batch-clearing/) | Batch Clearing | [UC-F04-01](../use-cases/UC-F04-01-run-batch-clearing/use-case.md) | [system](../use-cases/UC-F04-01-run-batch-clearing/sequences/SEQ-UC-F04-01-system.md) | [services](../../05-components/sequences/SEQ-F04-UC-F04-01-services.md) |
| [F-05](F-05-live-market-data/) | Live Market Data | [UC-F05-01](../use-cases/UC-F05-01-stream-market-data/use-case.md) | [system](../use-cases/UC-F05-01-stream-market-data/sequences/SEQ-UC-F05-01-system.md) | [services](../../05-components/sequences/SEQ-F05-UC-F05-01-services.md) |
| [F-06](F-06-positions-pnl-margin/) | Positions / PnL / Margin | [UC-F06-01](../use-cases/UC-F06-01-show-positions/use-case.md) | [system](../use-cases/UC-F06-01-show-positions/sequences/SEQ-UC-F06-01-system.md) | [services](../../05-components/sequences/SEQ-F06-UC-F06-01-services.md) |
| [F-07](F-07-pretrade-risk/) | Pre-trade Risk Control | [UC-F07-01](../use-cases/UC-F07-01-pretrade-risk-check/use-case.md) | [system](../use-cases/UC-F07-01-pretrade-risk-check/sequences/SEQ-UC-F07-01-system.md) | [services](../../05-components/sequences/SEQ-F07-UC-F07-01-services.md) |
| [F-08](F-08-posttrade-risk-and-liquidations/) | Post-trade Risk & Liquidations | [UC-F08-01](../use-cases/UC-F08-01-liquidate-position/use-case.md) | [system](../use-cases/UC-F08-01-liquidate-position/sequences/SEQ-UC-F08-01-system.md) | [services](../../05-components/sequences/SEQ-F08-UC-F08-01-services.md) |
| [F-09](F-09-batch-combo-orders/) | Combo & Portfolio Orders | [UC-F09-01](../use-cases/UC-F09-01-create-combo-order/use-case.md) | [system](../use-cases/UC-F09-01-create-combo-order/sequences/SEQ-UC-F09-01-system.md) | [services](../../05-components/sequences/SEQ-F09-UC-F09-01-services.md) |
| [F-10](F-10-mm-curves/) | Market-Maker Curves | [UC-F10-01](../use-cases/UC-F10-01-publish-mm-curve/use-case.md) | [system](../use-cases/UC-F10-01-publish-mm-curve/sequences/SEQ-UC-F10-01-system.md) | [services](../../05-components/sequences/SEQ-F10-UC-F10-01-services.md) |
| [F-11](F-11-external-venues-lob-to-fob/) | External Venues LOB → FOB | [UC-F11-01](../use-cases/UC-F11-01-ingest-external-marketdata/use-case.md) | [system](../use-cases/UC-F11-01-ingest-external-marketdata/sequences/SEQ-UC-F11-01-system.md) | [services](../../05-components/sequences/SEQ-F11-UC-F11-01-services.md) |
| [F-12](F-12-execution-hedge/) | Execution Hedge | [UC-F12-01](../use-cases/UC-F12-01-execute-hedge/use-case.md) | [system](../use-cases/UC-F12-01-execute-hedge/sequences/SEQ-UC-F12-01-system.md) | [services](../../05-components/sequences/SEQ-F12-UC-F12-01-services.md) |
| [F-13](F-13-posttrade-report/) | Post-Trade Report | [UC-F13-01](../use-cases/UC-F13-01-generate-posttrade-report/use-case.md) | [system](../use-cases/UC-F13-01-generate-posttrade-report/sequences/SEQ-UC-F13-01-system.md) | [services](../../05-components/sequences/SEQ-F13-UC-F13-01-services.md) |
| [F-14](F-14-deposit-withdraw/) | Deposit / Withdraw | [UC-F14-01](../use-cases/UC-F14-01-deposit-funds/use-case.md) | [system](../use-cases/UC-F14-01-deposit-funds/sequences/SEQ-UC-F14-01-system.md) | [services](../../05-components/sequences/SEQ-F14-UC-F14-01-services.md) |
| [F-15](F-15-backtest-replay/) | Backtest / Replay | [UC-F15-01](../use-cases/UC-F15-01-replay-historical-batch/use-case.md) | [system](../use-cases/UC-F15-01-replay-historical-batch/sequences/SEQ-UC-F15-01-system.md) | [services](../../05-components/sequences/SEQ-F15-UC-F15-01-services.md) |
| [F-16](F-16-operator-console/) | Operator Console & Kill-Switch | [UC-F16-01](../use-cases/UC-F16-01-trigger-kill-switch/use-case.md) | [system](../use-cases/UC-F16-01-trigger-kill-switch/sequences/SEQ-UC-F16-01-system.md) | [services](../../05-components/sequences/SEQ-F16-UC-F16-01-services.md) |
| [F-17](F-17-monitoring-and-alerts/) | Monitoring & Alerting | [UC-F17-01](../use-cases/UC-F17-01-fire-alert/use-case.md) | [system](../use-cases/UC-F17-01-fire-alert/sequences/SEQ-UC-F17-01-system.md) | [services](../../05-components/sequences/SEQ-F17-UC-F17-01-services.md) |

## Использование

- Бизнес-аналитик: открывает фичу → смотрит use case → видит system-level sequence.
- Архитектор: открывает service-level sequence → проверяет контракты в `docs/06-api/`.
- Разработчик: открывает feature.yaml → находит компонент в `docs/05-components/` → реализует.
- QA: открывает acceptance-criteria.md фичи → пишет тесты.

## Conflict Notes

**IN-001 vs feature-index numbering (зафиксировано 2026-05-13):**

Исходник IN-001 содержит две таблицы фич с разной нумерацией:

| Source table | F-15 | F-16 | F-17 | F-18 |
| --- | --- | --- | --- | --- |
| «Сводная таблица Feature» | Backtest/Replay | Operator Console & Kill-Switch | Monitoring & Alerts | — |
| «Трассировка фичей» | Перемещение коллатерала | Backtest & Replay | Operator Console | Monitoring & Alerts |

**Резолюция:** используется первая нумерация — она соответствует существующему коду (`specs/domain/feature-component-map.yaml`), service-level sequence diagrams и proto-контрактам. Содержание «Перемещение коллатерала» из второй таблицы вмержено в F-14 (Deposit/Withdraw) как раздел rebalance — это семантически близко (см. acceptance F-14 и `collateral_transfers.reason='rebalance'`).

Trace IN-001-FR-027, IN-001-FR-028.
