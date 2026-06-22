# Kafka Topic: fills

## Purpose

Per-fill события, извлечённые из `BatchResult` (matching). Используются Market Data
Service (F-05) для расчёта effective spread и downstream-аналитикой.

## Producer

- [matching-fob-core](../../05-components/matching-fob-core/overview.md)

## Consumers

- [market-data](../../05-components/market-data/overview.md) — effective spread (F-05)
- (reserved) downstream-аналитика

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 7 дней |
| Partition key | `order_id` / `fill_id` |
| Delivery | at-least-once, idempotent on `fill_id` |
| Schema | `fob.matching.v1.FillEvent` (Protobuf) |

## Message schema

См. [contracts/proto/fob/matching/v1/fill_event.proto](../../../contracts/proto/fob/matching/v1/fill_event.proto).

Ключевое поле `exec_price` — **VWAP-цена исполнения** (Σnotional / Σqty), не notional.

## Related

- Feature: F-05 Live Market Data
- Sequence: [SEQ-F05-UC-F05-01-services.md](../../05-components/sequences/SEQ-F05-UC-F05-01-services.md)
