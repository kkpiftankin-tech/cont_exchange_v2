---
id: DOC-IMPL-OVERVIEW
phase: 09-implementation
status: draft
owner: core-team
related:
  - cpp/
  - docs/05-components/components-overview.md
---

# Implementation Overview

## Языки и стек

- **C++20**, CMake-сборка (`CMakeLists.txt` + per-module `cpp/<module>/CMakeLists.txt`).
- gRPC + Protobuf для синхронных контрактов.
- librdkafka (через C++ wrapper) для Kafka.
- libpqxx для PostgreSQL, clickhouse-cpp для ClickHouse, hiredis для Redis.

## Карта модулей

| Модуль | Путь | Документ компонента |
| --- | --- | --- |
| common | [../../cpp/common/](../../cpp/common/) | [cpp-common.md](cpp-common.md) |
| gateway | [../../cpp/gateway/](../../cpp/gateway/) | [../05-components/gateway/](../05-components/gateway/) |
| order_flow | [../../cpp/order_flow/](../../cpp/order_flow/) | [../05-components/order-flow/](../05-components/order-flow/) |
| matching | [../../cpp/matching/](../../cpp/matching/) | [../05-components/matching-fob-core/](../05-components/matching-fob-core/) |
| risk | [../../cpp/risk/](../../cpp/risk/) | [../05-components/risk-manager/](../05-components/risk-manager/) |
| ledger | [../../cpp/ledger/](../../cpp/ledger/) | [../05-components/ledger/](../05-components/ledger/) |
| market_data | [../../cpp/market_data/](../../cpp/market_data/) | [../05-components/market-data/](../05-components/market-data/) |
| venues | [../../cpp/venues/](../../cpp/venues/) | [../05-components/external-venues/](../05-components/external-venues/) |
| observability | [../../cpp/observability/](../../cpp/observability/) | [../05-components/observability-reporting/](../05-components/observability-reporting/) |

## Конвенции

- Декомпозиция по слоям: `domain/`, `application/`, `infrastructure/`, `interfaces/`.
- Все денежные величины — `fob::common::Decimal` (см. ADR-005).
- События идут через Kafka в формате Protobuf, **at-least-once**, ручной commit.
- Все публичные сообщения — версионируются (`v1`, `v2`).

## Связанные документы

- [cpp-common.md](cpp-common.md) — детали общей библиотеки.
- [migration-map.md](migration-map.md) — соответствие legacy ↔ новые модули.
- [../../specs/domain/code-map.yaml](../../specs/domain/code-map.yaml) — машинная карта кода.
