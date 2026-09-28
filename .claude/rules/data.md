---
paths: ["infra/postgres/**", "docs/07-data/**"]
---

# Целевые хранилища данных (перенесено из CLAUDE.md §14, §25.4)

Текущий MVP использует in-memory состояние в ряде сервисов наряду с
PostgreSQL (`infra/postgres/init.sql` уже содержит много боевых таблиц — не
считай этот раздел чисто целевым состоянием, сверяйся с реальной схемой).

## PostgreSQL / OLTP

Целевые/основные таблицы:

- `users`;
- `sessions`;
- `accounts`;
- `flow_orders`;
- `positions`;
- `risk_limits`;
- `risk_snapshots`;
- `collateral_transfers`;
- `solver_config`;
- `hedgeflows` (F-12, включая колонки `reference_mid`, `hedge_pnl` — см.
  [`docs/04-domain/calculations/CALC-F12-HEDGE-PNL.md`](../../docs/04-domain/calculations/CALC-F12-HEDGE-PNL.md)).

PostgreSQL — источник истины для оперативного состояния: пользователи,
сессии, счета, активные заявки, позиции, лимиты, маржа, конфиги.

## ClickHouse / OLAP

Целевые таблицы:

- `fills`;
- `batch_results`;
- `marketdata`;
- `risk_events`;
- `execution_reports`;
- `agent_logs`;
- `hedge_pnl_agg` (F-12 DoD-6, см. `cpp/market_data/src/app/hedge_pnl_aggregator.cpp`).

ClickHouse — источник аналитики, истории, replay, quality-of-execution
reports и observability queries.

## Persistence changes

При добавлении persistence:

1. опиши схему в `docs/07-data/`;
2. создай migration strategy;
3. добавь репозиторий в `infra/` слоя сервиса;
4. покрой transactional boundaries;
5. добавь integration test с test DB;
6. обнови runbook и backup/retention docs.

## Чек-лист: добавить DB table

Навык `register-pg-table` автоматизирует шаги 1 и 4 (doc stub + `feature.yaml`
ссылку) для НОВОЙ таблицы:

1. Описать таблицу в `docs/07-data/`.
2. Описать owner service.
3. Описать migration.
4. Добавить repository/DAO.
5. Добавить integration tests.
6. Обновить backup/retention docs.

## Финансовая точность (напоминание — полное правило в CLAUDE.md §3)

Запрещено использовать `double`/`float` для денежных величин в domain-логике,
ledger, risk, matching settlement или persistence — только
`cex::common::Decimal` / protobuf `Decimal`. При конвертации: явно фиксируй
scale, не теряй sign, покрывай unit tests, не смешивай base amount, quote
amount и price без типов/явных имён.
