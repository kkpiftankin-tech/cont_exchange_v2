<!--
---
id: UC-F05A-04
title: "Validate Algorithm on Real Exchange Order Books"
level: sea
parent-feature: F-05A
system-sequence: "sequences/SEQ-UC-F05A-04-system.md"
service-sequence: "../../../05-components/sequences/SEQ-F05A-UC-F05A-04-services.md"
---
-->

# UC-F05A-04. Validate Algorithm on Real Exchange Order Books

## Feature

- [F-05A. Vectorized External Liquidity](../../features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Primary Actor

Researcher / Developer

## Supporting Actors

- Fixture store (`tests/fixtures/real-orderbooks/`), market-data, matching, CI

## Preconditions

- Captured real order book fixtures минимум из двух независимых venues (Binance / Coinbase / Kraken) с raw response + metadata + `raw_response_sha256`.
- Fixtures immutable; тест выполняется **offline** (без live API).

## Trigger

Прогон validation-теста (unit/integration) или ручной запуск через UI Replay.

## Main Flow

1. Выбрать captured fixture(s) (multi-venue / multi-pair, напр. BTC/USDT + ETH/USDT + ETH/BTC).
2. Загрузить raw order books → `ExternalOrderLevel[]`.
3. Запустить vectorization → `VectorFlowSegment[]` → `W`.
4. Запустить solver → `x`, `π`, `residual`.
5. Проверить `residualNorm < tolerance` **или** явный surplus.
6. Сравнить expected / actual (детерминированно); проверить source mapping (venue/source_order_id сохранены).

## Alternative Flows

### A1. Fixture невалиден (quality gate §8.8)

1. Отсутствует raw response / metadata / sha256 → тест fail на quality gate, не на алгоритме.

## Postconditions

- Доказано: алгоритм корректно строит `W`, `x` и балансирует активы на **реальных** данных.
- Fixtures и результаты версионированы; прогон воспроизводим и offline.

## Related Sequence Diagrams

- System sequence: [sequences/SEQ-UC-F05A-04-system.md](sequences/SEQ-UC-F05A-04-system.md)
- Service sequence: [../../../05-components/sequences/SEQ-F05A-UC-F05A-04-services.md](../../../05-components/sequences/SEQ-F05A-UC-F05A-04-services.md)

## Related Contracts

- Внешние venue order book APIs (Binance `/api/v3/depth`, Coinbase product book, Kraken `/Depth`) — только для refresh fixtures, не для CI.

## Related Components

- `market-data`, `matching`, test harness / CI

## Related Data

- `tests/fixtures/real-orderbooks/`; CH `vector_clearing_results` (при интеграционном прогоне)
