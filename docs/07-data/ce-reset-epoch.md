---
id: DOC-DATA-CE-RESET-EPOCH
phase: 07-data
status: implemented (2026-11-02, CR-F18-018 этап Э1)
level: sea
owner: core-team
source:
  - docs/02-system/features/F-18-ce-capital-position-hedge/feature.yaml (AC-F18v2-41, AC-F18v2-42)
related:
  - docs/03-architecture/adr/ADR-090-ce-positions-single-owner-uniform-reset.md
  - docs/06-api/rest/epochs.md
  - docs/06-api/rest/reset-positions.md
  - specs/domain/epoch-classes.yaml (классы хранилищ относительно метки)
  - frontend/api/epoch_schema.js (единственный источник текста DDL; init.sql сверяется тестом)
---

# Data: ce_reset_epoch (метка сброса позиций, эпоха)

## Назначение

Хранит метки сброса позиций агентов CE. Каждый потребитель истории (BFF, скрипты разбора, SQL-выжимки, выгрузки эксперту) определяет по ним, к какой эпохе относится интервал, и не смешивает
интервалы до и после сброса. Эпоха 0 виртуальная («до первой метки»), в таблице не хранится.

## Owner

- **Writer.** Функция `ce_epoch_open(...)` — единственный способ создать метку; её вызывают BFF (маршрут сброса, вид `bridge`) и `tools/reset/absolute_reset.sh` (вид `absolute`); с этапа Э2 — ledger.
  Досборка унаследованных эпох — `ce_epoch_backfill_legacy()` (один раз, пока таблица пуста).
- **Readers.** BFF (`loadEpochs`, кэш 3 с), `tools/lib/epochs.py`, SQL-функции `ce_epoch_at`, `ce_epoch_windows`, `ce_current_epoch`, `ce_guard_window`, представление `ce_epoch_intervals`.
- **DDL.** BFF создаёт схему сам (`ensureEpochSchema`, только `CREATE ... IF NOT EXISTS` / `CREATE OR REPLACE`, существующие таблицы не изменяются); тот же текст в `infra/postgres/init.sql`.

## PostgreSQL

| Колонка | Тип | Смысл |
| --- | --- | --- |
| `epoch_id` | BIGINT PK, > 0 | номер; идут подряд без дыр: `prev_epoch = epoch_id − 1`, `UNIQUE(prev_epoch)` исключает развилку |
| `reset_at` | TIMESTAMPTZ | момент по часам сервера БД, строго больше предыдущего, не из будущего (допуск 5 с) |
| `actor` | TEXT | автор из `X-User-Id` (пока не доказательство личности) |
| `kind` | TEXT | `absolute` / `bridge` / `legacy` |
| `note` | TEXT | название эксперимента, 1–200 знаков, без управляющих символов |
| `boot_id` | TEXT | запуск ledger на момент сброса (если известен) |
| `config_version` | BIGINT | версия настроек клиринга (эксперимент = эпоха + версия настроек) |
| `request_id` | TEXT UNIQUE | идемпотентность: повтор возвращает ту же эпоху |
| `before_state`, `initial_state` | JSONB | снимок состояния до сброса и исходное состояние; деньги — строками. Наружу через API не отдаются |

Индекс `ce_reset_epoch_at (reset_at)` уникальный. Триггеры запрещают UPDATE, DELETE и TRUNCATE и проверяют монотонность и «не из будущего».

## Функции

| Функция | Что делает |
| --- | --- |
| `ce_epoch_open(actor, kind, note, request_id, before, initial, boot_id, config_version, reset_at)` | блокировка, повтор по `request_id`, следующий номер, момент `GREATEST(часы, предыдущая + 1 мкс)`, не чаще раза в минуту (кроме `legacy`) |
| `ce_epoch_at(ts)` | номер эпохи момента `ts` (0 — до первой метки) |
| `ce_current_epoch()` | номер текущей эпохи |
| `ce_epoch_windows(from, to)` | эпохи, затронутые окном, с границами, обрезанными по окну |
| `ce_guard_window(from, to, reason)` | отвергает окно через метку без причины; `true`, если пересекает с причиной |
| `ce_epoch_backfill_legacy()` | унаследованные эпохи из `f05a_clearing_config` и её истории |

## Retention и связь с историей

Метки не удаляются (аудит). Журналы истории (`child_orders`, `ce_agent_pnl_history`, `ce_pnl_totals_history`, `ce_agent_band_history`, `ce_system_pnl_history`, таблицы ClickHouse) связываются с
эпохой по времени (`ce_epoch_at`); колонка `epoch_id` при записи в четыре истории BFF — этап Э2 (эпоха берётся из того же ответа ledger, что и данные). Классы таблиц —
`specs/domain/epoch-classes.yaml`. ClickHouse хранит 7 суток: разбор эпохи старше недели по его таблицам неполон (OQ-F18v2-64).

## Проверка

`frontend/api/testdata/epoch_schema_check.sh` выполняет схему и сценарий проверок на настоящем Postgres в одной транзакции во временной схеме и откатывает её (рабочая база не меняется).
