# UC-F12-01. Хеджирование через внешнюю площадку

## Feature

- [F-12. Execution Hedge](../../features/F-12-execution-hedge/)

## Primary Actor

System (Risk / Matching → Venues)

## Supporting Actors

- CEX / DEX

## Preconditions

- Накоплена net-позиция, превышающая внутренний inventory limit.
- Venues adapter сконфигурирован и доступен.

## Trigger

Risk / Matching формирует `ExecutionIntent`.

## Main Flow

1. Источник публикует `execution.intents` с параметрами хедж-сделки.
2. Venues consumer получает intent.
3. Venues посылает child order на внешний venue.
4. Venue возвращает confirmation / partial fill / reject.
5. Venues публикует `execution.reports`.
6. Ledger применяет hedge fill к internal accounting.
7. Observability логирует.

## Alternative Flows

### A1. Reject от venue — повтор или эскалация.
### A2. Timeout — retry с backoff, alert если превышены попытки.

## Postconditions

- Внешняя сделка зафиксирована.
- Позиция выровнена.
- Audit trail.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F12-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F12-UC-F12-01-services.md)

## Related Contracts

- [execution.intents](../../../06-api/messaging/execution-intents.md)
- [execution.reports](../../../06-api/messaging/execution-reports.md)
- `fob.execution.v1.ExecutionIntent`, `ExecutionReport`

## Related Components

- [external-venues](../../../05-components/external-venues/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [ledger](../../../05-components/ledger/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)

## Related Data

- (планируется) `execution_reports` в ClickHouse
- [positions](../../../07-data/data-overview.md)
