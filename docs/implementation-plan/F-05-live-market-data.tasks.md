# F-05 Live Market Data — Implementation Tasks

## Feature

[F-05](../02-system/features/F-05-live-market-data/)

## Status: MVP-in-progress → Full Implementation

Текущее состояние: базовый ticker cache работает. Необходимо реализовать полный
F-05 функционал: ComputeMarketData, ComputeEffectiveSpread, GetReferencePrices,
WebSocket stream, ClickHouse persistence.

---

## Блок 1: Proto / Contracts

- [x] T-05-C1: Создать `contracts/proto/fob/marketdata/v1/marketdata_service.proto`
      — MarketDataSnapshot, MarketDataUpdate, ReferencePrice, GetReferencePrices*,
        GetMarketDataSnapshot*, GetMarketDataHistory*, GetEffectiveSpread*, SubscribeMarketData*
- [x] T-05-C2: Расширить `MarketDataService` в `marketdata_raw.proto` новыми RPC-методами
- [ ] T-05-C3: Пересобрать proto (`cmake --build build`), убедиться что нет ошибок компиляции
- [ ] T-05-C4: Обновить `contracts/CMakeLists.txt` если нужны новые источники

---

## Блок 2: Infrastructure / ClickHouse DDL

- [ ] T-05-I1: Создать DDL-файл `infra/clickhouse/V001__marketdata_snapshots.sql`
      — таблица `marketdata_snapshots` (см. docs/07-data/marketdata-snapshots.md)
- [ ] T-05-I2: Создать DDL-файл `infra/clickhouse/V002__effective_spreads.sql`
      — таблица `effective_spreads` (см. docs/07-data/effective-spreads.md)
- [ ] T-05-I3: Создать DDL-файл `infra/postgresql/V001__marketdata_config.sql`
      — таблица `marketdata_config` + seed data (BTCUSDT, ETHUSDT, SOLUSDT)
- [ ] T-05-I4: Обновить `infra/kafka/create_topics.sh`
      — добавить `marketdata.snapshots` (retention 7d, partition by asset)
- [ ] T-05-I5: Обновить `infra/docker-compose.dev.yml`
      — добавить ClickHouse сервис (если отсутствует), env vars для market_data

---

## Блок 3: Domain / App logic

- [ ] T-05-A1: Создать `cpp/market_data/src/domain/market_data_snapshot.hpp`
      — value object MarketDataSnapshot (без proto-зависимостей)
- [ ] T-05-A2: Создать `cpp/market_data/src/domain/effective_spread_record.hpp`
      — value object EffectiveSpreadRecord
- [ ] T-05-A3: Создать `cpp/market_data/src/domain/ports/i_snapshot_storage.hpp`
      — порт ISnapshotStorage (SaveSnapshot, GetLatestSnapshot, GetHistory)
- [ ] T-05-A4: Создать `cpp/market_data/src/domain/ports/i_effective_spread_storage.hpp`
      — порт IEffectiveSpreadStorage (SaveRecord, GetHistory)
- [ ] T-05-A5: Создать `cpp/market_data/src/domain/ports/i_market_data_config.hpp`
      — порт IMarketDataConfig (GetConfig, IsActive)
- [ ] T-05-A6: Создать `cpp/market_data/src/app/compute_market_data.hpp/.cpp`
      — алгоритм ComputeMarketData (bestBid/bestAsk из aggregate curves, mid, spread, depth, volume)
- [ ] T-05-A7: Создать `cpp/market_data/src/app/compute_effective_spread.hpp/.cpp`
      — алгоритм ComputeEffectiveSpread из FillEvent
- [ ] T-05-A8: Расширить `MarketDataUseCases` в `market_data_uc.hpp/.cpp`:
      — добавить OnFillEvent(), GetMarketDataSnapshot(), GetReferencePrices()
      — добавить in-memory cache last_snapshot_ (per asset, thread-safe)
      — добавить fallback logic при пустом рынке

---

## Блок 4: Infrastructure (ClickHouse persistence)

- [ ] T-05-P1: Создать `cpp/market_data/src/infra/clickhouse/ch_snapshot_storage.hpp/.cpp`
      — реализация ISnapshotStorage через ClickHouse HTTP client
      — INSERT INTO marketdata_snapshots, SELECT latest/history
- [ ] T-05-P2: Создать `cpp/market_data/src/infra/clickhouse/ch_effective_spread_storage.hpp/.cpp`
      — реализация IEffectiveSpreadStorage через ClickHouse HTTP client
- [ ] T-05-P3: Создать `cpp/market_data/src/infra/postgres/pg_market_data_config.hpp/.cpp`
      — реализация IMarketDataConfig через PostgreSQL
      — SELECT из marketdata_config, кэшировать с TTL 30s

---

## Блок 5: Kafka

- [ ] T-05-K1: Создать `cpp/market_data/src/infra/kafka_fills_consumer.hpp/.cpp`
      — consumer для топика `fills`, вызывает `OnFillEvent()`
- [ ] T-05-K2: Создать `cpp/market_data/src/infra/kafka_snapshots_producer.hpp/.cpp`
      — producer для топика `marketdata.snapshots`
      — partition key = asset
- [ ] T-05-K3: Обновить `cpp/market_data/src/infra/kafka_consumer.hpp/.cpp`
      — убедиться что `batch.outputs` consumer вызывает новый `ComputeMarketData`
- [ ] T-05-K4: Обновить `cpp/market_data/src/main.cpp`
      — подключить новые consumers/producers, storage, config

---

## Блок 6: gRPC transport

- [ ] T-05-G1: Реализовать `GetMarketDataSnapshot` в `grpc_market_data_service.cpp`
      — читает из in-memory cache (fast path), fallback из ClickHouse
- [ ] T-05-G2: Реализовать `GetReferencePrices` в `grpc_market_data_service.cpp`
      — batch read из cache, fallback при stale > staleThreshold
- [ ] T-05-G3: Реализовать `GetMarketDataHistory` в `grpc_market_data_service.cpp`
      — запрос к ClickHouse с фильтром по (asset, from, to, limit)
- [ ] T-05-G4: Реализовать `GetEffectiveSpread` в `grpc_market_data_service.cpp`
- [ ] T-05-G5: Реализовать `SubscribeMarketData` (server streaming) в `grpc_market_data_service.cpp`
      — первое сообщение — полный snapshot, далее delta через BroadcastUpdate

---

## Блок 7: Risk alert

- [ ] T-05-R1: В `ComputeMarketData` добавить проверку spreadBps > threshold
      — при превышении публиковать `RiskAlert` в Kafka `risk.alerts`
      — threshold читается из `marketdata_config.spread_alert_threshold_bps`

---

## Блок 8: Tests

- [ ] T-05-T1: Unit тесты U1–U10 для `compute_market_data` и `compute_effective_spread`
      — файл `cpp/market_data/tests/compute_market_data_test.cpp`
- [ ] T-05-T2: Integration тест I1 (BatchResult → snapshot в ClickHouse + WS push)
- [ ] T-05-T3: Integration тест I2 (FillEvent → effective_spreads)
- [ ] T-05-T4: Integration тест I6 (GetReferencePrices)

---

## Зависимости

| Блок | Зависит от |
|------|-----------|
| Блок 3 (domain) | Блок 1 (proto) |
| Блок 4 (infra) | Блок 3 (domain) |
| Блок 5 (kafka) | Блок 3 (domain) |
| Блок 6 (gRPC) | Блок 3 + 4 + 5 |
| Блок 7 (risk) | Блок 3 + 5 |
| Блок 8 (tests) | Все предыдущие |

## Приоритет MVP

Минимально необходимое для демо:
1. T-05-C3 (сборка proto)
2. T-05-I1, T-05-I2, T-05-I3 (DDL)
3. T-05-A6, T-05-A7, T-05-A8 (алгоритмы)
4. T-05-K1, T-05-K2, T-05-K3 (Kafka)
5. T-05-G1, T-05-G2 (GetMarketDataSnapshot + GetReferencePrices)
