# UC-F11-01. Принять external market data (LOB → FOB)

## Feature

- [F-11. External Venues LOB → FOB](../../features/F-11-external-venues-lob-to-fob/)

## Primary Actor

System (Venues adapter)

## Supporting Actors

- CEX / DEX (внешняя площадка)

## Preconditions

- Сконфигурирована площадка и инструмент.

## Trigger

Внешняя площадка шлёт стакан / тики / сделки.

## Main Flow

1. Venues adapter получает LOB-снапшот.
2. Adapter преобразует LOB → CSLO кривую (`p_low`, `p_high`, `q_max`, `q_rate`).
3. Adapter публикует `marketdata.raw`.
4. Market Data Service обновляет кэш тикеров.
5. (Опционально) FOB-кривая включается в matching через `orders.normalized`.

## Alternative Flows

### A1. Сбой подключения к площадке

1. Adapter ретрайит, alert в `risk.alerts`.

## Postconditions

- Внешняя ликвидность представлена в FOB-форме.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F11-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F11-UC-F11-01-services.md)

## Related Contracts

- [marketdata.raw](../../../06-api/messaging/marketdata-raw.md)
- (планируется) `orders.normalized` с источником `venue`

## Related Components

- [external-venues](../../../05-components/external-venues/overview.md)
- [market-data](../../../05-components/market-data/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)

## Related Data

- (планируется) `marketdata` table в ClickHouse
