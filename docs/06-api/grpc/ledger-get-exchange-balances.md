# gRPC Method: LedgerService/GetExchangeBalances

## Status

Implemented (ADR-054 §10; поле `boot_id` добавлено 2026-10-22, ADR-084).

## Purpose

Валютный вектор биржи: по каждой валюте собственные средства (house-аккаунт `__ce_house__`), средства на внешних биржах (чистые результаты хеджей и переводов), клиентские обязательства и in-flight. Из них `risk` считает $NOP_a$, а BFF — остатки $q=\text{assets\_total}-\text{client\_liability}$ для блока «Система по остаткам» ([CALC-CE-SYSTEM-PNL](../../04-domain/calculations/CALC-CE-SYSTEM-PNL.md)).

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc GetExchangeBalances(GetExchangeBalancesRequest) returns (GetExchangeBalancesResponse);
```

## Schema (изменение 2026-10-22)

```proto
message GetExchangeBalancesResponse {
  fob.common.v1.EventMeta meta = 1;
  repeated ExchangeCurrencyBalance balances = 2;
  string house_account_id = 3;   // '__ce_house__'
  string boot_id = 4;            // идентификатор запуска ledger (UUID v4), новый при каждом перезапуске
}
```

`boot_id` аддитивное поле: старые потребители его не читают. Остатки внешних бирж ledger хранит в памяти и обнуляет при перезапуске; потребитель, заметивший смену `boot_id`, задаёт новую базу вместо ложного торгового результата (BFF — [system-pnl](../rest/system-pnl.md), ADR-084 п. 8).

## Related ADR

- [ADR-054](../../03-architecture/adr/ADR-054-ce-capital-net-position-hedging.md), [ADR-084](../../03-architecture/adr/ADR-084-ce-link-physical-transfer.md)
