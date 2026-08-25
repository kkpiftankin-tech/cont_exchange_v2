# Компонент: external-venues-connector

> **Runtime:** `cpp/venues` (один бинарь с Normalizer + Curve Builder + Execution Adapter).
>
> **Conflict Note:** Спецификация IN-004 описывает Connector как отдельный компонент; в коде он живёт внутри `cpp/venues` бинаря (см. C-1 в [F-11 README](../../02-system/features/F-11-external-venues-lob-to-fob/README.md#conflict-notes) и [ADR-014](../../03-architecture/adr/ADR-014-venues-binary-vs-components.md)).

## Назначение

Низкоуровневый адаптер к внешним торговым площадкам. Поддерживает три семейства транспортов:

- **CEX WS+REST** (Binance, Coinbase): [cex_ws_rest_adapter.cpp](../../../cpp/venues/src/infra/cex_ws_rest_adapter.cpp). Локальная сборка LOB из инкрементов — [cex_local_lob_assembler.cpp](../../../cpp/venues/src/infra/cex_local_lob_assembler.cpp).
- **DEX/AMM RPC** (Uniswap v3): [dex_amm_rpc_adapter.cpp](../../../cpp/venues/src/infra/dex_amm_rpc_adapter.cpp). Подписка на events + чтение pool state.
- **Simulator** (для CI/dev): [simulated_venue_adapter.cpp](../../../cpp/venues/src/infra/simulated_venue_adapter.cpp), [venue_sim_adapter.cpp](../../../cpp/venues/src/infra/venue_sim_adapter.cpp).

Внешний интерфейс — [`VenueAdapter`](../../../cpp/venues/src/domain/venue_adapter.hpp) с операциями: connect / subscribe / poll / reconnect / disconnect, плюс heartbeat callbacks.

## Что делает (MVP)

1. Подключение по конфигу из `venue_config` (PostgreSQL) или env-fallback.
2. Heartbeat: каждый цикл публикует RAW `VenueHealth` в Kafka `venue.health` через [VenueObservabilityProducer](../../../cpp/venues/src/infra/venue_observability_producer.cpp).
3. Reconnect с backoff (`circuit_breaker_*` параметры подсказывают, когда сдаваться).
4. Отдаёт сырые сообщения в [Venue Market Data Normalizer](../venue-market-data-normalizer/overview.md).
5. Stale detection: если данных нет более `STALE_THRESHOLD_MS` — выставляет `VenueConnectionStatus::kStale`.

## Конфигурация

| env                                | Default          | Назначение                                                  |
| ---------------------------------- | ---------------- | ----------------------------------------------------------- |
| `VENUES_ADAPTER_MODE`              | `multi_real`     | `multi_real`/`cex_ws_rest`/`dex_amm_rpc`/`simulator`        |
| `KAFKA_BROKERS`                    | `redpanda:9092`  | Kafka                                                       |
| `VENUES_POSTGRES_DSN`              | (unset)          | если задан — конфиг площадок из `venue_config`              |
| `BINANCE_VENUE_ID`/`*_VENUE_SYMBOL` | (in compose)     | дефолтные площадки для multi_real                           |
| `STALE_THRESHOLD_MS`               | `15000`          | порог stale                                                 |
| `CIRCUIT_BREAKER_ERRORS`           | `3`              | подсказка к Health Service                                  |

## Связанные фичи

- F-11 (этот компонент — её primary).
- F-12 (Execution Hedge) — использует тот же бинарь для отправки child orders.

## Participates In Features

- [F-11](../../02-system/features/F-11-external-venues-lob-to-fob/), [F-12](../../02-system/features/F-12-execution-hedge/), [F-05](../../02-system/features/F-05-live-market-data/) (через legacy `marketdata.raw`)

## Participates In Use Cases

- [UC-F11-01](../../02-system/use-cases/UC-F11-01-onboard-venue/use-case.md), [UC-F11-02](../../02-system/use-cases/UC-F11-02-publish-snapshot/use-case.md), [UC-F11-04](../../02-system/use-cases/UC-F11-04-venue-health-degradation/use-case.md), [UC-F11-05](../../02-system/use-cases/UC-F11-05-execute-hedge-on-venue/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F11-01-onboard-venue-services](../sequences/SEQ-F11-01-onboard-venue-services.md)
- [SEQ-F11-02-publish-snapshot-services](../sequences/SEQ-F11-02-publish-snapshot-services.md)
- [SEQ-F11-04-health-routing-services](../sequences/SEQ-F11-04-health-routing-services.md)
- [SEQ-F11-05-execute-on-venue-services](../sequences/SEQ-F11-05-execute-on-venue-services.md)

## Produced Events

- [venue.health (RAW)](../../06-api/messaging/venue-topics.md#venue-health)
- [marketdata.raw (legacy)](../../06-api/messaging/marketdata-raw.md)

## Consumed Events

- (нет — Connector только инициатор соединения; ExecutionIntent consume — см. venue-execution-adapter)

## Data Access

- [venue_config](../../07-data/venue-config.md) (PostgreSQL, R через VenuesLoop)
