# ADR-011 — replay_agentlogs DDL placement

- **Status:** Accepted (текущая реализация)
- **Date:** 2026-05-20
- **Deciders:** core-team
- **Source:** F-15 ingest (IN-006) — Conflict Note CN-IN006-04

## Контекст

ClickHouse-таблица `replay_agentlogs` имеет два возможных места
определения DDL:

1. **Static init**: `infra/clickhouse/init.sql` — DDL применяется один раз
   при инициализации контейнера `clickhouse` через docker-compose
   `command`/`init`. Все остальные F-04 таблицы (`batchresults`, `fills`)
   определены именно так.
2. **Runtime EnsureSchema**: `cpp/backtest/src/infra/clickhouse_storage.cpp`
   метод `EnsureSchema()` — DDL применяется при старте сервиса
   `backtest`, `CREATE TABLE IF NOT EXISTS`.

Текущая реализация использует вариант 2.

## Решение

Сохранить **вариант 2 (runtime EnsureSchema)** для `replay_agentlogs`.

## Обоснование

- **Configurable retention.** Таблица создаётся с TTL = `${BACKTEST_CLICKHOUSE_REPLAY_AGENTLOGS_RETENTION_DAYS}` (default 90). При изменении env переменной сервис применяет `ALTER MODIFY TTL` при следующем старте. В static init.sql это было бы сложнее: хардкод или sed-инъекция.
- **Schema evolution.** ALTER ADD COLUMN IF NOT EXISTS используется для миграции (например, `log_id`, `session_config_snapshot_version`). Идемпотентно при каждом запуске.
- **Ownership локальное.** `replay_agentlogs` — приватная таблица backtest-service. Производственный путь не зависит от неё, F-04 / F-11 / F-13 потребляют только `batchresults`, `fills`, `marketdata_snapshots`.
- **Bootstrap order.** clickhouse контейнер healthy до старта backtest, поэтому EnsureSchema гарантированно отработает при первом старте.

## Trade-offs / последствия

- **Не виден в `infra/clickhouse/init.sql` для review.** Реверс-инжиниринг
  DDL требует чтения C++ кода. Mitigation: документация
  [docs/07-data/replay-agentlogs.md](../../07-data/replay-agentlogs.md)
  фиксирует актуальную DDL.
- **Сервис не запустится до `clickhouse.healthy=true`.** Уже отражено в
  `docker-compose.dev.yml` `depends_on.clickhouse.condition: service_healthy`.
- **Schema drift risk** между документом и кодом. Mitigation: ADR-008
  (Code/Doc drift policy) — изменение DDL в `clickhouse_storage.cpp`
  обязано сопровождаться обновлением `docs/07-data/replay-agentlogs.md`.

## Альтернативы

- **Вариант 1 (static init.sql).** Отклонён: нет configurable retention,
  schema evolution через скрипты усложняет CI.
- **Migration tool (golang-migrate / dbmate).** Overkill для одной
  таблицы в одном сервисе.

## Обратимость

Reversible. При необходимости (например, если потребуется единая
clickhouse-инициализация для compliance):

1. Дублировать CREATE TABLE в `infra/clickhouse/init.sql` с
   хардкоженным TTL = 90.
2. Удалить вызов `EnsureSchema()` для `replay_agentlogs` из
   `clickhouse_storage.cpp` (оставить только для ALTER TTL).

См. task T-F15-005 в [implementation-plan/F-15-backtest-replay.tasks.md](../../implementation-plan/F-15-backtest-replay.tasks.md).

## Связанные артефакты

- Код: [cpp/backtest/src/infra/clickhouse_storage.cpp](../../../cpp/backtest/src/infra/clickhouse_storage.cpp) lines 362–405
- Документ: [docs/07-data/replay-agentlogs.md](../../07-data/replay-agentlogs.md)
- Conflict Note: CN-IN006-04 в [incoming-docs/IN-006.fragment-map.md](../../../incoming-docs/IN-006.fragment-map.md)

## Source

- F-15 ingest review, IN-006 § replay_agentlogs DDL
