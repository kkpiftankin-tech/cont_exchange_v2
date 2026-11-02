---
id: DOC-DATA-LIQUIDITY-CURVES-CH
phase: 07-data
status: implemented
level: sea
owner: core-team
source:
  - cpp/market_data/src/infra/clickhouse/clickhouse_liquidity_curve_storage.cpp
related:
  - docs/07-data/venue-liquidity-curves.md (кривые отдельной биржи, таблица venue_liquidity_curves)
---

# ClickHouse: `liquidity_curves` (агрегатные кривые ликвидности)

## Назначение и владельцы

Хранит кривые ликвидности (стороны bid и ask в JSON) по каждой бирже и паре, которые `market_data` получает от `venues` и использует для векторного клиринга. Пишет `market_data` (`ClickHouseLiquidityCurveStorage`), читает BFF (`frontend/api/server.js`, агрегатные кривые FOB и график кривой).

## Схема и хранение

```sql
CREATE TABLE default.liquidity_curves (
  timestamp_ms Int64, venue_id String, symbol String, base String, quote String,
  snapshot_id String, curve_id String, mid_price Float64, -- и остальные скалярные поля кривой
  bid_curve_json String, ask_curve_json String,
  ingested_at DateTime DEFAULT now()
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(toDateTime(intDiv(timestamp_ms, 1000)))
ORDER BY (timestamp_ms, venue_id, symbol)
TTL toDateTime(timestamp_ms / 1000) + toIntervalDay(<срок хранения>)
SETTINGS index_granularity = 8192, ttl_only_drop_parts = 1;
```

Срок хранения задаёт переменная `CLICKHOUSE_LIQUIDITY_CURVES_RETENTION_DAYS` (деф. 90; на dev 7 по решению владельца 2026-10-21). `market_data` при старте выставляет TTL таблицы по этой переменной (`ALTER TABLE ... MODIFY TTL`), поэтому менять срок только в базе бессмысленно.

## Объём и почему секции

Строка весит около 84 КБ (в основном JSON кривых), за сутки на dev приходится 15–160 тыс. строк, то есть 1–13 ГБ. Без секций TTL-слияние каждые 4 часа (`merge_with_ttl_timeout`) переписывало одну часть на 110 GiB, чтобы убрать строки старше срока хранения, и держало ClickHouse на 370 % CPU при 32 % ожидания диска (2026-10-21; цепочка клиринга деградировала). С секциями по дню и `ttl_only_drop_parts = 1` истёкшие сутки удаляются целой секцией без перезаписи.

## Миграция существующей таблицы

`CREATE TABLE IF NOT EXISTS` не меняет уже созданную таблицу, а ключ секционирования нельзя поменять на месте. Миграция (выполнена на dev 2026-10-21): создать `liquidity_curves_new` с нужным DDL, скопировать строки за срок хранения окнами по 6 часов с малым размером блока (`max_block_size=2048`, иначе строки по 84 КБ дают блоки в гигабайты и нехватку памяти), атомарно подменить (`EXCHANGE TABLES`), докопировать записанное в старую таблицу за время копирования (по `ingested_at`), сверить счётчики и удалить старую таблицу. Фактически на dev: скопировано 629 869 строк за 7 дней (35 строк первого часа окна «состарились» и ушли по TTL за время копирования), докопировано 29 129, итог 660 тыс. строк, 59 GiB вместо 124 GiB; копирование заняло 2,5 часа при перегруженном ClickHouse. Старая таблица осталась под именем `liquidity_curves_new` до ручного удаления владельцем.
