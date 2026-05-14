# gRPC Method: CustodyService/CreateDepositAddress

## Status

TODO / planned

## Purpose

Создать (или вернуть уже существующий) адрес для депозита пользователя в указанной валюте/сети. Custody-adapter управляет HD-кошельками и мониторингом on-chain транзакций; после подтверждения транзакции вызывает `LedgerService/ApplyDeposit`.

## Transport

gRPC

## Service

`fob.custody.v1.CustodyService` (planned package; компонент `custody-adapter` сейчас отсутствует)

## Method (planned)

```proto
rpc CreateDepositAddress(CreateDepositAddressRequest) returns (CreateDepositAddressResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `POST /v1/deposits/address` → gRPC)

## Callee

- `custody-adapter` (planned — компонент не создан) (planned)

## Schema

TODO. Предлагаемая форма:

```proto
message CreateDepositAddressRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  string currency = 3;                   // "BTC", "USDT"
  string network = 4;                    // "bitcoin", "ethereum", "tron"
}

message CreateDepositAddressResponse {
  fob.common.v1.EventMeta meta = 1;
  string address = 2;
  string memo = 3;                       // for memo/tag networks
  google.protobuf.Timestamp expires_at = 4;
  fob.common.v1.Error error = 5;
}
```

## Idempotency

По `(user_id, currency, network)`. Повторный вызов возвращает тот же адрес, пока он не expired.

## Security

- Private keys никогда не покидают custody-adapter.
- Адреса whitelist-блокируются если custody-policy этого требует.

## Used In Features

- [F-14. Deposit / Withdraw](../../02-system/features/F-14-deposit-withdraw/)

## Used In Use Cases

- [UC-F14-01](../../02-system/use-cases/UC-F14-01-deposit-funds/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F14-UC-F14-01-services](../../05-components/sequences/SEQ-F14-UC-F14-01-services.md)

## Related Components

- `custody-adapter` (planned — компонент не создан) (planned)
- [gateway](../../05-components/gateway/overview.md)
- [ledger](../../05-components/ledger/overview.md) (downstream `ApplyDeposit`)

## Related Data Objects

- [`collateral_transfers`](../../07-data/oltp-schema.md#таблица-collateral_transfers)
- HD-wallet state (внутреннее хранилище custody)

## Source Fragments

- IN-001-FR-016 (FR-CUST-002 deposit-address, derived)
- IN-001-FR-022
