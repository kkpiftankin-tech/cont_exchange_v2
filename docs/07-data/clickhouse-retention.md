---
id: DOC-DATA-CLICKHOUSE-RETENTION
phase: 07-data
status: implemented (dev, 2026-10-21/22); срок 7 дней временный
level: sea
owner: core-team
source:
  - решение владельца 2026-10-21: исторические данные клиринга старше недели не нужны, удаление автоматическое
related:
  - docs/07-data/liquidity-curves-clickhouse.md (таблица liquidity_curves, секции по дню)
  - docs/07-data/olap-schema.md
---

# ClickHouse: автоматическое удаление данных старше 7 дней

## Решение

Все аналитические данные в ClickHouse (базы `default` и `backtest`) пока хранятся не дольше 7 дней (решение владельца 2026-10-22; до этого 2026-10-21 так ограничили данные клиринга). Удаление автоматическое: у каждой таблицы задан TTL (срок жизни строки), ClickHouse сам убирает истёкшие строки при фоновых слияниях (не реже раза в 4 часа на таблицу, настройка `merge_with_ttl_timeout`). TTL хранится в метаданных таблицы и переживает перезапуски сервисов и контейнера. Ручные удаления больше не нужны.

## Таблицы и срок

Срок 7 дней у всех таблиц ниже, кроме двух короче. Выражение TTL для колонки с миллисекундами (`event_time_ms`): `toDateTime(intDiv(event_time_ms, 1000)) + INTERVAL 7 DAY`; для колонки `timestamp` (DateTime64): `toDateTime(timestamp) + INTERVAL 7 DAY`.

| База | Таблицы | Как задан срок |
| --- | --- | --- |
| `default` | `liquidity_curves` (секции по дню, удаление целыми секциями) | `market_data` при старте по `CLICKHOUSE_LIQUIDITY_CURVES_RETENTION_DAYS` |
| `default` | `vector_clearing_results`, `batchresults`, `batch_metrics`, `execution_venue`, `venue_snapshots`, `venue_liquidity_curves` | `ALTER TABLE ... MODIFY TTL` на dev (2026-10-21) |
| `default` | `OrderBook`, `execution_reports`, `marketdata_snapshots`, `effective_spreads`, `fills`, `fill_metrics`, `backtest_batchresults`, `backtest_fills`, `grouped_execution_events`, `grouped_leg_fills`, `grouped_quality_metrics`, `grouped_ratio_deviation`, `sim_execution_reports`, `sim_divergence_log`, `hedge_pnl_agg`, `venue_quality_reports` | `ALTER TABLE ... MODIFY TTL` на dev (2026-10-22) |
| `default` | `replay_agentlogs` | `backtest` при старте по `BACKTEST_CLICKHOUSE_REPLAY_AGENTLOGS_RETENTION_DAYS` |
| `backtest` | `venue_snapshots` | `venues` при старте по `VENUES_CLICKHOUSE_RETENTION_DAYS` |
| `default` | `vector_flow_segments_history` | 2 дня, `market_data` при старте |
| `backtest` | `venue_book_live` | 2 часа, `venues` при старте |

Применение на существующей таблице: `ALTER TABLE <база>.<таблица> MODIFY TTL <выражение>`; по умолчанию ClickHouse сразу запускает мутацию материализации TTL и убирает уже накопленные старые строки. Для новых окружений те же сроки заданы в `infra/clickhouse/init.sql`.

Три сервиса (`market_data`, `venues`, `backtest`) выставляют TTL своих таблиц при каждом старте по переменным окружения (`CLICKHOUSE_LIQUIDITY_CURVES_RETENTION_DAYS`, `VENUES_CLICKHOUSE_RETENTION_DAYS`, `BACKTEST_CLICKHOUSE_REPLAY_AGENTLOGS_RETENTION_DAYS`); на dev и в `infra/env/.env-example` они равны 7. Без этого перезапуск вернул бы TTL по умолчанию (90 дней).

## Что не затронуто

- Системные таблицы ClickHouse (`system.text_log`, `query_log`, `trace_log`, `part_log`, `processors_profile_log`) копят сотни миллионов строк; их срок задаётся конфигурацией сервера (`config.d`), это внутренние журналы, а не аналитические данные, и они вне этого решения.
- Данные PostgreSQL (оперативное состояние, `venue_fill_diagnostics` и др.): срок не менялся.

## Как позже увеличить срок

Срок 7 дней временный: владелец (2026-10-22) оставил возможность хранить историю дольше. Чтобы не править 27 таблиц вручную, есть скрипт `tools/clickhouse-retention/set_retention.sh <дней> [--apply]` (на хосте с ClickHouse; без `--apply` только печатает `ALTER TABLE ... MODIFY TTL`). Порядок:

1. Заранее: данные, которые уже истекли по текущему TTL, не вернуть, поэтому увеличивать срок нужно до того, как нужный период выйдет за 7 дней.
2. Выполнить скрипт с нужным числом дней и `--apply`.
3. Поставить те же дни в переменных окружения `CLICKHOUSE_LIQUIDITY_CURVES_RETENTION_DAYS`, `VENUES_CLICKHOUSE_RETENTION_DAYS`, `BACKTEST_CLICKHOUSE_REPLAY_AGENTLOGS_RETENTION_DAYS` (dev: конец `infra/env/.env-example` на хосте; репозиторий: `infra/env/.env-example`) и пересоздать `market_data`, `venues`, `backtest`, иначе их старт вернёт прежнее значение.
4. Для новых окружений поправить `INTERVAL` в `infra/clickhouse/init.sql`.
5. Учесть объём: `liquidity_curves` весит около 8–13 ГБ в сутки (≈84 КБ на строку), срок 30 дней означает порядка 250–400 ГБ на диске ClickHouse; остальные таблицы малы (до нескольких сотен МБ за 7 дней).

## Ограничения и эксплуатация

- Таблицы с TTL по колонке `event_time_ms` без секций переписываются при TTL-слиянии целиком; для малых таблиц (до 1 ГБ) это допустимо. Для таблиц с большим объёмом строк используйте секции по дню и `ttl_only_drop_parts = 1` (как у `liquidity_curves`): иначе TTL-слияние раз в 4 часа переписывает самую большую часть (2026-10-21 так перегрузили ClickHouse, см. liquidity-curves-clickhouse.md).
- Удаление таблиц больше 50 ГБ ClickHouse блокирует предохранителем `max_table_size_to_drop`; для разового `DROP` передайте `?max_table_size_to_drop=0` в запросе.
- Нужен срок дольше недели для разбора инцидента: поднять TTL нужной таблицы до начала нужного периода (`MODIFY TTL ... INTERVAL N DAY`) до того, как данные истекут: удалённое не вернуть.
