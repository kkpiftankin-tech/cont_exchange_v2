---
id: DOC-DATA-MIGRATIONS
phase: 07-data
status: living
level: sea
owner: core-team
related:
  - docs/07-data/ce-agent-pnl-book.md
  - docs/07-data/ce-agent-pnl-history.md
  - docs/07-data/ce-pnl-totals-history.md
  - docs/07-data/ce-agent-position.md
---

# Data: журнал изменений схемы PostgreSQL (живые тома)

`infra/postgres/init.sql` применяется только при создании тома. На существующем томе схему меняет сервис-владелец при старте идемпотентным DDL (`CREATE … IF NOT EXISTS`, проверка каталога перед `ALTER`, короткий `lock_timeout`). Здесь — записи об изменениях, влияющих на живые тома; новая запись добавляется при каждой миграции, затрагивающей существующую таблицу.

## 2026-10-29 — ce_agent_band_history (ADR-088, CR-F18-013)

- **Владелец DDL:** frontend-api (BFF), `ensureBandHistoryReady` (`CREATE TABLE IF NOT EXISTS` и индекс при первой записи); дубль в `infra/postgres/init.sql` (свежий том).
- **Изменения:** новая таблица `ce_agent_band_history` ([документ](ce-agent-band-history.md)) — минутная история рабочих и модельных порогов хеджа, позиции и оценок потока по каждому агенту. Существующие таблицы не затрагиваются. Умолчание колонки `f05a_clearing_config.ce_maker_fee_bps` для НОВЫХ томов изменено с 2 на 10 (ADR-089); существующую строку это не меняет.
- **Безопасность:** DDL идемпотентен; блокировок существующих таблиц нет. Запись идёт отдельным try минутного прохода BFF и не влияет на историю заработка.
- **Откат:** `CE_HMODEL_MODE=off` (записи прекращаются) или `DROP TABLE IF EXISTS ce_agent_band_history;` — внешних ключей, представлений и триггеров нет.

## 2026-10-28 — ce_pnl_totals_history и внутренняя группа в ce_agent_pnl_history (CR-F18-005)

- **Владелец DDL:** frontend-api (BFF), `ensurePnlHistoryTable` / `ensurePnlHistoryReady`; дубль в `infra/postgres/init.sql` (свежий том).
- **Изменения:** новая таблица `ce_pnl_totals_history` ([документ](ce-pnl-totals-history.md)); колонки `internal_realized_usdt`, `internal_unrealized_usdt` в `ce_agent_pnl_history` (`NUMERIC(38,12)`, `NULL` у прежних строк; добавление колонки без значения по умолчанию меняет только каталог).
- **Безопасность:** каталог читается в текущей схеме; `ALTER` и индекс выполняются в одной транзакции с `SET LOCAL lock_timeout = '3s'`; при таймауте транзакция откатывается, повтор — со следующего вызова.
- **Откат:** удалить таблицу `ce_pnl_totals_history` и две новые колонки `ce_agent_pnl_history` (команды `DROP TABLE` и `ALTER TABLE … DROP COLUMN IF EXISTS`); обе таблицы только для отображения, ledger их не читает.

## 2026-10-28 — ce_agent_pnl_book: модель A → C′ (ADR-087, CR-F18-004)

- **Владелец DDL:** ledger (`EnsurePnlSchema`, один раз на процесс), дубль в `infra/postgres/init.sql`.
- **Изменения:** колонки `avg_price` → `avg_fair`, `realized` → `edge`, добавлены `ext_realized`, `quote`; строки модели A удалены (значения в C′ не пересчитываются). Колонки `ext_*`, `rvf_base` таблицы `ce_agent_position` не используются и остаются.
- **Безопасность и откат:** [ce-agent-pnl-book](ce-agent-pnl-book.md), раздел «Миграция» (независимые идемпотентные шаги, блокировка с перепроверкой стража; откат — удаление таблицы `ce_agent_pnl_book`, старый ledger пересоздаст её).
