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
| `ce_maker_fee_bps` | NUMERIC(38,18) | 10 | комиссия мейкера на внешней бирже без спреда, б.п. (стандарт 0,1 %, ADR-089; ledger читает значение ≤ 0 как «не задано») |
| `ce_band_q_floor` | NUMERIC(38,18) | 5 | пол ширины, тыс. USDT |
| `ce_band_gamma_mode` | BOOLEAN | false | автоматический режим по волатильности |
| `ce_band_sigma_ref` | NUMERIC(38,18) | 0,0005 | опорная σ |
| `ce_band_vol_ratio_min` / `ce_band_vol_ratio_max` | NUMERIC(38,18) | 0,1 / 10 | границы множителя волатильности |
| `ce_band_manual` | BOOLEAN | false | ручная ширина включена (ADR-081) |
| `ce_band_manual_q_lim` | NUMERIC(38,18) | 5 | Z̄lim вручную, тыс. USDT |
| `ce_band_manual_q_mkt` | NUMERIC(38,18) | 25 | Z̄mkt вручную, тыс. USDT |
| `ce_band_arb_scale` | NUMERIC(38,18) | 1 | множитель порога выхода арбитражёров (ADR-081); допустимо 0,1…20, иначе `ledger` берёт 1 |
| `ce_hmodel_mode` | TEXT, `CHECK IN ('off','shadow','on')`, NULL допустим | NULL | режим модели порогов хеджа (ADR-088, условие 1). NULL = «не задан»: ledger считает `shadow`. Эффективный режим в ledger = меньший из потолка окружения `CE_HMODEL_MODE`, этого значения и предохранителя, поэтому `on` здесь рабочий режим не включает при низком потолке |

Правило режима: `ce_band_manual = true` и значения допустимы (0,1 ≤ Z̄lim ≤ Z̄mkt ≤ 1000 тыс. USDT) — ручной; иначе `ce_band_gamma_mode = true` — по волатильности; иначе от комиссии. Недопустимые ручные значения при включённом режиме `ledger` игнорирует и один раз пишет предупреждение.

Если ручные колонки не удалось прочитать (обрыв соединения или их нет), `ledger` сохраняет последнее известное значение режима, а не выключает его.

Миграция существующего тома: `ALTER TABLE f05a_clearing_config ADD COLUMN IF NOT EXISTS …` для четырёх колонок ADR-081 (три ручных и `ce_band_arb_scale`) и `CREATE TABLE IF NOT EXISTS operator_audit_log …` (они же в `infra/postgres/init.sql`).

**`ce_hmodel_mode`: кто читает и пишет.** Читает ledger (`ReadHedgeModePgModeLocked`, кэш 5 с, поверх кэш настроек 1 с — смена действует до ≈ 6 с). Пишут два писателя: BFF (оператор, `POST /api/vector-clearing/config`, в одной транзакции с `operator_audit_log`, прежнее значение читается под `SELECT … FOR UPDATE`) и ledger (автопредохранитель: `UPDATE … SET ce_hmodel_mode='shadow', updated_at=now() WHERE id=1 AND ce_hmodel_mode='on'` и строка аудита, и то и другое — одной транзакцией; аудит пишется, только если строка действительно изменилась). Любое изменение значения поднимает `config_version` и архивирует прежнюю строку в `f05a_clearing_config_history` (триггер версионирования). Не задавать значение прямым `UPDATE`: это обойдёт аудит. Колонку нужно создать до выкладки новых ledger и BFF (на томе без колонки ledger считает `shadow`, а `POST` с этим полем даёт `502` и откатывается целиком).

## Таблица `operator_audit_log` — журнал операторских действий

Пишут BFF (в одной транзакции с изменением настроек) и ledger (автопредохранитель, актор `ledger:auto-fuse`); по соглашению только `INSERT`, без правок и удалений. Нужна для аудита операторских действий (CLAUDE.md §1.7).

| Колонка | Тип | Смысл |
| --- | --- | --- |
| `id` | BIGSERIAL, PK | порядковый номер |
| `ts` | TIMESTAMPTZ, по умолчанию `now()` | время записи |
| `actor` | TEXT | заголовок `X-User-Id` или `unknown` (клиент выставляет сам — не доказательство личности) |
| `action` | TEXT | действие: `ce_band_width_change`, `ce_clearing_config_change`, `ce_hmodel_mode_change` (оператор), `ce_hmodel_fuse_trip` (автопредохранитель ledger) |
| `remote_addr` | TEXT | адрес клиента (`X-Real-IP`, `X-Forwarded-For` или адрес сокета) |
| `user_agent` | TEXT | `User-Agent`, до 300 символов |
| `details` | JSONB | форма BFF: `{ts, level, msg, user, changed:{поле:{from,to}}}` (для режима модели порогов — `changed.ce_hmodel_mode`, при включении после срабатывания предохранителя `confirmed_after_fuse: true`); форма ledger: `{trigger:'auto_fuse', agent_id, exposure_k, limit_k, over_ms, from:'on', to:'shadow'}` |

Индекс `idx_operator_audit_log_ts` по времени (по убыванию). Политика хранения не задана: для аудита записи не удаляются автоматически.
