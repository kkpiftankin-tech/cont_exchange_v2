# Компонент: venue-market-data-normalizer

> **Runtime:** `cpp/venues` (общий бинарь, см. ADR-009).

## Назначение

Принимает сырые ордербуки/трейды/pool-state от [External Venues Connector](../external-venues-connector/overview.md) и нормализует их в `fob.venue.v1.VenueSnapshot`:

- канонизирует depth: фильтрация дробных «пылевых» уровней, агрегация по `tick_size`/`lot_size`, упорядочивание ([depth_canonicalizer.cpp](../../../cpp/venues/src/domain/depth_canonicalizer.cpp));
- вычисляет $\text{mid} = \frac{\text{bestBid} + \text{bestAsk}}{2}$ и $\text{spread} = \text{bestAsk} - \text{bestBid}$;
- определяет `status` (connected/stale/empty/disconnected) ([snapshot_status.cpp](../../../cpp/venues/src/domain/snapshot_status.cpp));
- заполняет `fees` (maker/taker), `volume_24h`, `tick_size`, `lot_size`.

Публикует `VenueSnapshot` в Kafka `venue.snapshots` через [SnapshotProducer](../../../cpp/venues/src/app/snapshot_producer.cpp). Параллельно пишет в ClickHouse `venue_snapshots` через [SnapshotClickHouseWriter](../../../cpp/venues/src/infra/snapshot_clickhouse_writer.cpp).

## Алгоритм нормализации

1. Получить raw depth (bid/ask уровни) + last trade.
2. Канонизировать: отсев dust (`qty < lot_size * threshold`), агрегация соседних уровней по `tick_size`.
3. Вычислить $\text{bestBid}$, $\text{bestAsk}$, $\text{mid}$, $\text{spread}$.
4. Определить статус по timestamp последнего обновления.
5. Сформировать `VenueSnapshot` с `EventMeta` (correlation_id, partition_key=`{venue}|{symbol}`).
6. Опубликовать.

## Конфигурация

| env                                | Default          | Назначение                                  |
| ---------------------------------- | ---------------- | ------------------------------------------- |
| `VENUES_CLICKHOUSE_URL`            | `http://clickhouse:8123` | ClickHouse endpoint                  |
| `VENUES_CLICKHOUSE_DB`             | `backtest`       | Схема БД                                    |
| `VENUES_CLICKHOUSE_SNAPSHOTS_TABLE`| `venue_snapshots`| Таблица                                     |
| `VENUES_CLICKHOUSE_RETENTION_DAYS` | `90`             | Retention (≥ 90 по NFR)                     |
| `STALE_THRESHOLD_MS`               | `15000`          | Порог stale-статуса                         |

## Связанные фичи

- F-11 (primary).
- F-05 (live market data) — потребитель `venue.snapshots` (постепенная миграция с `marketdata.raw`).

## Participates In Features

- [F-11](../../02-system/features/F-11-external-venues-lob-to-fob/), [F-05](../../02-system/features/F-05-live-market-data/)

## Participates In Use Cases

- [UC-F11-02](../../02-system/use-cases/UC-F11-02-publish-snapshot/use-case.md), [UC-F11-04](../../02-system/use-cases/UC-F11-04-venue-health-degradation/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F11-02-publish-snapshot-services](../sequences/SEQ-F11-02-publish-snapshot-services.md)

## Produced Events

- [venue.snapshots](../../06-api/messaging/venue-topics.md#venue-snapshots)

## Consumed Events

- (внутренний канал от Connector — не Kafka)

## Data Access

- [venue_snapshots](../../07-data/venue-snapshots.md) (ClickHouse, W)
