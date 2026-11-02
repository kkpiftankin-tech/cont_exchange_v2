---
id: DOC-DATA-CE-SYSTEM-PNL
phase: 07-data
status: implemented (2026-10-22, ADR-083)
level: sea
owner: core-team
source:
  - docs/02-system/features/F-18-ce-capital-position-hedge/feature.yaml (AC-F18v2-26)
related:
  - docs/04-domain/calculations/CALC-CE-SYSTEM-PNL.md (формулы: торговый результат и переоценка запасов)
  - docs/06-api/rest/system-pnl.md (маршрут, из которого читает экран)
---

# Data: ce_system_pnl_baseline и ce_system_pnl_history (прибыль системы по остаткам)

## Назначение

Хранят базу и ряд прибыли системы CE по реальным остаткам биржи. Остатки берутся из ledger (`GetExchangeBalances`: активы минус обязательства перед клиентами), цены — из последних сегментов клиринга. Изменение стоимости остатков с момента базы делится на **торговый результат** и **переоценку запасов** ([CALC-CE-SYSTEM-PNL](../04-domain/calculations/CALC-CE-SYSTEM-PNL.md)). Таблицы только для отображения: ledger, risk и matching их не читают.

## Owner

- **Writer: frontend-api (BFF).** Собственный семплер `runSystemPnlSample` (раз в `PNL_HISTORY_SAMPLE_MS`, первый проход через 5 с после старта, таймаут прохода 20 с), независимый от истории агентов: зависший ledger или ClickHouse не задерживает запись агентов. Сбой замера только логируется (`[system-pnl] sample failed`). Таблицы создаются лениво при первом проходе. Остатки и цены кэшируются на 10 с.
- **Когда точка не пишется.** Нет остатков из ledger или цен из ClickHouse (ошибка пробрасывается, нули не подставляются); у валюты с остатком нет свежей цены (сегмент клиринга старше 3 минут не используется) — тогда на графике не было бы ступеньки; базу задать нельзя (`decideBaseline` вернул `wait`).
- **База.** Строка `id = 1` в `ce_system_pnl_baseline`: момент замера, метка сброса статистики сделок (`reset_ms`), остатки `q0` и цены `m0` по валютам. База задаётся, только когда у каждой валюты с остатком есть цена (иначе переоценка запасов навсегда осталась бы нулевой), и заменяется, когда метка сброса в `f05a_clearing_config.trade_stats_reset_at` отличается от сохранённой; при сбросе через BFF строка базы удаляется и новая задаётся при ближайшем замере (до минуты). Маршрут показывает ряд с метки последнего сброса статистики сделок (при перезапуске ledger база меняется, но ряд непрерывен: точки включают перенос).
- **Rebase при перезапуске ledger.** Остатки бирж ledger хранит в памяти и обнуляет при перезапуске. Если `boot_id` ответа отличается от сохранённого в базе (или в базе его нет), семплер задаёт новую базу (`q0`, `m0` — сейчас) и переносит накопленное в `carry` — значения последней записанной точки ряда с момента прежней базы. Точки ряда после rebase включают перенос, график непрерывен. Сброс статистики сделок обнуляет перенос.
- **Reader: frontend-api**, `GET /api/vector-clearing/system-pnl` (контракт — `docs/06-api/rest/system-pnl.md`).
- Хранение: строки истории старше `PNL_HISTORY_RETENTION_DAYS` (деф. 7) удаляются раз в час.

## PostgreSQL (OLTP)

```sql
CREATE TABLE IF NOT EXISTS ce_system_pnl_baseline (
  id       INTEGER PRIMARY KEY CHECK (id = 1),
  ts       TIMESTAMPTZ NOT NULL,
  reset_ms BIGINT NOT NULL,
  q0       JSONB NOT NULL,
  m0       JSONB NOT NULL,
  boot_id  TEXT,                                   -- запуск ledger на момент базы (ADR-084)
  carry    JSONB NOT NULL DEFAULT '{}'::jsonb      -- {"trading": …, "revaluation": …}, перенесено через перезапуск ledger
);
CREATE TABLE IF NOT EXISTS ce_system_pnl_history (
  ts               TIMESTAMPTZ PRIMARY KEY,
  trading_usdt     NUMERIC(38, 12) NOT NULL,
  revaluation_usdt NUMERIC(38, 12) NOT NULL,
  mtm_change_usdt  NUMERIC(38, 12) NOT NULL,
  mtm_usdt         NUMERIC(38, 12) NOT NULL,
  skipped          INTEGER NOT NULL DEFAULT 0
);
```

| Поле | Смысл | Единицы |
| --- | --- | --- |
| `q0`, `m0` | остатки и цены (USDT за единицу валюты) на момент базы, по валютам | количество, USDT |
| `boot_id` | идентификатор запуска ledger, при котором задана база; другой `boot_id` в ответе `GetExchangeBalances` = ledger перезапущен | строка |
| `carry` | накопленные торговый результат и переоценка, перенесённые через перезапуск ledger (последняя точка ряда на момент rebase) | USDT |
| `trading_usdt` | торговый результат $T=\sum (q-q_0)\,m$ на момент `ts` | USDT |
| `revaluation_usdt` | переоценка запасов $V=\sum q_0\,(m-m_0)$ | USDT |
| `mtm_change_usdt` | $T+V$, изменение стоимости остатков с момента базы | USDT |
| `mtm_usdt` | стоимость остатков $\sum q\,m$ | USDT |
| `skipped` | сколько валют с остатком не оценено из-за отсутствия цены | штуки |

## Как читать ряд

- Ряд начинается с базы (первый замер или сброс статистики); до неё прошлое не восстанавливается.
- $T$ без сделок не меняется; движение рынка меняет только $V$.
- Остатки, цены и обязательства клиентов на dev — засеянные; сравнивать нужно изменения, а не уровни.
- Комиссии не учтены.

## Миграция

Таблицы создаёт сам BFF при первом проходе (`CREATE TABLE IF NOT EXISTS`) и они же лежат в `infra/postgres/init.sql`. Для существующего тома dev отдельная миграция не нужна.

## Тип денег

Колонки — `NUMERIC`, а не `double`. Значения вычисляет BFF в числах JavaScript из остатков ledger; это отображаемая метрика, а не проводка.
