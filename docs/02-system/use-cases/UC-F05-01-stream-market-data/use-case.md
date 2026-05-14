# UC-F05-01. Получить поток live market data

## Feature

- [F-05. Live Market Data](../../features/F-05-live-market-data/)

## Primary Actor

Trader / Market Maker

## Preconditions

- Пользователь аутентифицирован.
- Symbol торгуется.

## Trigger

Trader подписывается на market data поток.

## Main Flow

1. Trader открывает WebSocket / gRPC stream.
2. Gateway проверяет подписку.
3. Market Data Service шлёт начальный snapshot.
4. По мере поступления `marketdata.raw` обновления стримятся клиенту.

## Alternative Flows

### A1. Symbol не поддерживается

1. Возврат `NOT_FOUND`.

## Postconditions

- Trader получает поток `Ticker / Trade / OrderBookL2Update`.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F05-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F05-UC-F05-01-services.md)

## Related Contracts

- `fob.marketdata.v1.MarketDataService` — [../../../06-api/grpc/](../../../06-api/grpc/)
- [marketdata.raw](../../../06-api/messaging/marketdata-raw.md)

## Related Components

- [gateway](../../../05-components/gateway/overview.md)
- [market-data](../../../05-components/market-data/overview.md)
- [external-venues](../../../05-components/external-venues/overview.md)

## Related Data

- ticker cache в Redis
- (планируется) `marketdata` table в ClickHouse
