---
id: ADR-004
status: accepted
date: 2026-05-13
owners:
  - architecture
related:
  - docs/03-architecture/adr/ADR-001-event-driven-microservices.md
  - docs/03-architecture/adr/ADR-011-replay-agentlogs-ddl-placement.md
  - docs/07-data/oltp-schema.md
  - docs/07-data/olap-schema.md
  - infra/postgres/init.sql
  - infra/clickhouse/init.sql
---

# ADR-004: PostgreSQL (OLTP) + ClickHouse (OLAP) как data platform

## Контекст

Системе нужны две принципиально разные модели данных:

- **Transactional / mutable state**: users, sessions, accounts, flow_orders,
  positions, risk_limits, hedgeflows, child_orders, solver_config,
  sim_sessions. Сильная консистентность, частые UPDATE, FK, транзакции.
- **Analytics / history / time-series**: fills, batchresults,
  marketdata, execution_reports, sim_execution_reports, sim_divergence_log,
  replay_agentlogs. Append-heavy, большие объёмы, аналитические запросы,
  retention/TTL.

Один движок плохо обслуживает оба профиля. По факту в репозитории уже:
PostgreSQL (`infra/postgres/init.sql`) для OLTP и ClickHouse
(`infra/clickhouse/init.sql`) для OLAP. Этот ADR фиксирует разделение и
общую политику DDL.

## Решение

- **PostgreSQL — source of truth для mutable state** (OLTP). Операционные
  таблицы создаются через миграции / static init SQL.
- **ClickHouse — immutable append / history / analytics** (OLAP). Time-series,
  event history, quality-of-execution отчёты, replay/backtest аналитика.
- Бизнес-решения **не** принимаются напрямую из (потенциально отстающего)
  ClickHouse, если это явно не помечено как допустимое.

### Политика DDL

| Тип таблицы | Где создаётся DDL |
| --- | --- |
| Shared production (OLTP и общие OLAP — accounts, positions, fills, batchresults, execution_reports, sim_*) | миграции / static init SQL (`infra/postgres/init.sql`, `infra/clickhouse/init.sql`) |
| Private service-owned OLAP (например `replay_agentlogs`) | runtime `EnsureSchema()` сервиса-владельца (см. [ADR-011](ADR-011-replay-agentlogs-ddl-placement.md)) |

Runtime `EnsureSchema` допустим **только** для приватных таблиц одного
сервиса; общие production-таблицы используют static DDL/миграции.

## Альтернативы

- **Только PostgreSQL** (включая аналитику) — отклонено: плохо масштабируется на time-series и аналитические сканы fills/marketdata.
- **Только ClickHouse** — отклонено: нет транзакций/FK/UPDATE для mutable OLTP state.
- **PostgreSQL + отдельный TSDB (Timescale/Influx)** — отклонено: ClickHouse сильнее для произвольной OLAP-аналитики и уже в стеке.

## Последствия

### Положительные

- Каждый движок работает в своём профиле; предсказуемая производительность.
- Чёткая граница: mutable state vs immutable history.

### Отрицательные

- Два хранилища = две операционные модели (backup, retention, мониторинг).
- Дублирование части данных (state в PG, история в CH) и необходимость ingestion-пути PG/Kafka → CH.

## Обратимость

Низкая. Перенос аналитики в другой движок потребует переписать ingestion
и read-side (Backtest, Replay, Diagnostics). Разделение OLTP/OLAP —
устоявшийся выбор и менять его не планируется.
