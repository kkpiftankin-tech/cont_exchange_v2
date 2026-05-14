# UC-F13-01. Получить post-trade отчёт

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
