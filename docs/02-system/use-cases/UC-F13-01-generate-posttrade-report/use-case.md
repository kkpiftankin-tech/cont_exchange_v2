<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F13-01. Получить post-trade отчёт

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-13-posttrade-report](../../features/F-13-posttrade-report/) |
| ☁️ L0 system sequence | [SEQ-UC-F13-01-system](sequences/SEQ-UC-F13-01-system.md) — system как чёрный ящик |
| 🌊 L1 service sequence | [SEQ-F13-UC-F13-01-services](../../../05-components/sequences/SEQ-F13-UC-F13-01-services.md) — взаимодействие сервисов |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-13. Post-Trade Report](../../features/F-13-posttrade-report/)

## Primary Actor

Trader / Compliance

## Preconditions

- Период имеет завершённые fills.

## Trigger

Trader запрашивает отчёт за период.

## Main Flow

1. Trader посылает `GetPostTradeReport(period)` через gateway.
2. Observability агрегирует данные из ClickHouse: fills, batch_results, VWAP, IS.
3. Сервис возвращает отчёт (JSON / CSV / PDF).

## Postconditions

- Отчёт сформирован.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F13-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F13-UC-F13-01-services.md)

## Related Contracts

- (планируется) `GET /v1/reports/post-trade`
- `fob.observability.v1` — обзор контрактов

## Related Components

- [gateway](../../../05-components/gateway/overview.md)
- [observability-reporting](../../../05-components/observability-reporting/overview.md)

## Related Data

- (планируется) `fills`, `batch_results` в ClickHouse
