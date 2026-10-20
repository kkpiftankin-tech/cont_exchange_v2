---
id: DOC-DATA-F05A-CLEARING-CONFIG-BAND
phase: 07-data
status: draft
owner: core-team
source:
  - ADR-081
  - ADR-066
related:
  - infra/postgres/init.sql
  - cpp/ledger/src/app/ledger_uc.cpp
---

# Таблица `f05a_clearing_config` — колонки ширины полосы хеджа

Одна строка (`id = 1`). Колонки ширины полосы читает `ledger` (`LoadBandFeeConfig`, кэш около 1 с); пишет BFF (`POST /api/vector-clearing/config`) или оператор прямым запросом. Отсутствие колонок на старом томе допустимо: `ledger` читает их отдельным запросом и остаётся на расчётной ширине.

| Колонка | Тип | По умолчанию | Смысл |
| --- | --- | --- | --- |
| `ce_band_fee_k` | NUMERIC(38,18) | 1,8 | коэффициент ширины от комиссии (ADR-066), `Z̄ = k · комиссия` |
| `ce_maker_fee_bps` | NUMERIC(38,18) | 2 | комиссия мейкера, б.п. |
| `ce_band_q_floor` | NUMERIC(38,18) | 5 | пол ширины, тыс. USDT |
| `ce_band_gamma_mode` | BOOLEAN | false | автоматический режим по волатильности |
| `ce_band_sigma_ref` | NUMERIC(38,18) | 0,0005 | опорная σ |
| `ce_band_vol_ratio_min` / `ce_band_vol_ratio_max` | NUMERIC(38,18) | 0,1 / 10 | границы множителя волатильности |
| `ce_band_manual` | BOOLEAN | false | ручная ширина включена (ADR-081) |
| `ce_band_manual_q_lim` | NUMERIC(38,18) | 5 | Z̄lim вручную, тыс. USDT |
| `ce_band_manual_q_mkt` | NUMERIC(38,18) | 25 | Z̄mkt вручную, тыс. USDT |
| `ce_band_arb_scale` | NUMERIC(38,18) | 1 | множитель порога выхода арбитражёров (ADR-081); допустимо 0,1…20, иначе `ledger` берёт 1 |

Правило режима: `ce_band_manual = true` и значения допустимы (0,1 ≤ Z̄lim ≤ Z̄mkt ≤ 1000 тыс. USDT) — ручной; иначе `ce_band_gamma_mode = true` — по волатильности; иначе от комиссии. Недопустимые ручные значения при включённом режиме `ledger` игнорирует и один раз пишет предупреждение.

Если ручные колонки не удалось прочитать (обрыв соединения или их нет), `ledger` сохраняет последнее известное значение режима, а не выключает его.

Миграция существующего тома: `ALTER TABLE f05a_clearing_config ADD COLUMN IF NOT EXISTS …` для четырёх колонок ADR-081 (три ручных и `ce_band_arb_scale`) и `CREATE TABLE IF NOT EXISTS operator_audit_log …` (они же в `infra/postgres/init.sql`).

## Таблица `operator_audit_log` — журнал операторских действий

Пишет BFF в одной транзакции с изменением настроек; по соглашению только `INSERT`, без правок и удалений. Нужна для аудита операторских действий (CLAUDE.md §1.7).

| Колонка | Тип | Смысл |
| --- | --- | --- |
| `id` | BIGSERIAL, PK | порядковый номер |
| `ts` | TIMESTAMPTZ, по умолчанию `now()` | время записи |
| `actor` | TEXT | заголовок `X-User-Id` или `unknown` (клиент выставляет сам — не доказательство личности) |
| `action` | TEXT | действие, сейчас `ce_band_width_change` |
| `remote_addr` | TEXT | адрес клиента (`X-Real-IP`, `X-Forwarded-For` или адрес сокета) |
| `user_agent` | TEXT | `User-Agent`, до 300 символов |
| `details` | JSONB | режим до и после, изменившиеся поля (прежнее → новое) |

Индекс `idx_operator_audit_log_ts` по времени (по убыванию). Политика хранения не задана: для аудита записи не удаляются автоматически.
