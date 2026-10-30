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

## 2026-10-29 — ce_hmodel_mode и сырые пороги истории (ADR-088, поправка о сглаживании, CR-F18-014)

- **Владельцы DDL.** `f05a_clearing_config.ce_hmodel_mode` — самомигрирующего владельца нет (таблицу создаёт `init.sql`, как и прежние колонки ширины полосы): колонку **вручную создать до выкладки** новых ledger и BFF. Колонки `b_m_raw`, `b_t_raw`, `transfer_threshold_raw`, `transfer_target_raw` в `ce_agent_band_history` — владелец BFF (`ensureBandHistoryReady`), дубль в `init.sql`.
- **Порядок на живом томе.** (0) убедиться, что нет долгих транзакций: `SELECT pid, now()-xact_start AS age, left(query,60) FROM pg_stat_activity WHERE xact_start IS NOT NULL AND pid<>pg_backend_pid();`. (1) В отдельных транзакциях с `SET LOCAL lock_timeout='3s'`: `ALTER TABLE f05a_clearing_config ADD COLUMN IF NOT EXISTS ce_hmodel_mode TEXT CONSTRAINT f05a_clearing_config_hmodel_mode CHECK (ce_hmodel_mode IN ('off','shadow','on'));` и `ALTER TABLE IF EXISTS ce_agent_band_history ADD COLUMN IF NOT EXISTS b_m_raw NUMERIC(38,12), ADD COLUMN IF NOT EXISTS b_t_raw NUMERIC(38,12), ADD COLUMN IF NOT EXISTS transfer_threshold_raw NUMERIC(38,12), ADD COLUMN IF NOT EXISTS transfer_target_raw NUMERIC(38,12);`. (2) Проверка: `SELECT ce_hmodel_mode, config_version FROM f05a_clearing_config WHERE id=1;` (NULL, версия прежняя). (3) Потом выкатывать ledger и BFF. Режим вручную `UPDATE` не ставить: это обойдёт аудит.
- **Безопасность.** Все `ADD COLUMN` без значения по умолчанию — только каталог, без перезаписи таблицы и перестройки индексов (в `ce_agent_band_history` ≈ 2 млн строк при хранении 14 суток). Единственный риск — очередь за долгим запросом (часовая очистка, выгрузка CSV до 50 000 строк, ручной `psql`): поэтому `lock_timeout` и проверка длинных транзакций. Триггер версионирования при `ADD COLUMN` не срабатывает (версия конфигурации не растёт).
- **Совместимость.** Новый ledger на старой схеме: чтение режима падает, режим `shadow` (безопасно). Новый BFF на старой схеме: `GET` терпит (поля нет), `POST` с `ce_hmodel_mode` даёт `502` и откатывается целиком. Старые ledger/BFF колонку не знают, значение сохраняется.
- **Откат.** Сначала вернуть прежние образы ledger и BFF (или `CE_HMODEL_MODE=off`), затем `ALTER TABLE f05a_clearing_config DROP COLUMN IF EXISTS ce_hmodel_mode;` (ограничение уходит с колонкой, заданный режим теряется) и `ALTER TABLE ce_agent_band_history DROP COLUMN IF EXISTS b_m_raw, DROP COLUMN IF EXISTS b_t_raw, DROP COLUMN IF EXISTS transfer_threshold_raw, DROP COLUMN IF EXISTS transfer_target_raw;`. Новый BFF вернёт сырые колонки при старте, поэтому откатывать его первым.

## 2026-10-29 — ce_agent_band_history (ADR-088, CR-F18-013)

- **Владелец DDL:** frontend-api (BFF), `ensureBandHistoryReady` (`CREATE TABLE IF NOT EXISTS` и индекс при первой записи); дубль в `infra/postgres/init.sql` (свежий том).
- **Изменения:** новая таблица `ce_agent_band_history` ([документ](ce-agent-band-history.md)) — минутная история рабочих и модельных порогов хеджа, позиции и оценок потока по каждому агенту. Существующие таблицы не затрагиваются. Умолчание колонки `f05a_clearing_config.ce_maker_fee_bps` для НОВЫХ томов изменено с 2 на 10 (ADR-089); существующую строку это не меняет.
- **Безопасность:** таблица новая, поэтому блокировок существующих таблиц на этом шаге нет. Запись идёт отдельным try минутного прохода BFF и не влияет на историю заработка. Колонки сырых порогов, добавленные позже, — см. запись выше.
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
