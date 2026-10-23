---
id: DOC-DATA-CE-AGENT-PNL-HISTORY
phase: 07-data
status: implemented (2026-10-22; колонки выгоды добавлены 2026-10-22, ADR-083)
level: sea
owner: core-team
source:
  - docs/02-system/features/F-18-ce-capital-position-hedge/feature.yaml (AC-F18v2-23, AC-F18v2-25)
related:
  - docs/04-domain/calculations/CALC-CE-TRANSLATOR-PNL.md (что такое R и U у переводчика)
  - docs/04-domain/calculations/CALC-CE-ARBITRAGEUR-BASIS.md (R и U у арбитражёра)
  - docs/04-domain/calculations/CALC-CE-CLEARING-SURPLUS.md (выгода участника в клиринге)
  - docs/06-api/rest/agent-pnl-history.md (маршрут, из которого читает график)
  - docs/07-data/child-orders.md (источник сделок, по которым считаются R и U)
---

# Data: ce_agent_pnl_history (история заработка и выгоды агентов CE)

## Назначение

Хранит, как менялись во времени показатели каждого агента CE, чтобы по клику на агента в панели «Живые позиции» показать график. Запись — раз в минуту. Два вида величин:

- **R и U** — заработок по состоявшимся внешним сделкам (R — реализованный, U — нереализованный, USDT, без комиссий): те же числа, что в колонке «заработок R / U»; накопительные с момента сброса статистики сделок;
- **выгода в клиринге** (`surplus_inc_usdt`) — теоретическая выгода участника $S = w^2/(2\alpha)$, накопленная по тактам клиринга за минуту (приращение, не накопительная): [CALC-CE-CLEARING-SURPLUS](../04-domain/calculations/CALC-CE-CLEARING-SURPLUS.md). Она есть у каждого участника с потоком, в том числе у связок, у которых после ADR-083 внешних сделок нет.

Таблица только для отображения: ledger, risk и matching её не читают, в проводки она не попадает.

## Owner

- **Writer: frontend-api (BFF).** Семплер в `frontend/api/server.js` (`startPnlHistorySampler`) каждые `PNL_HISTORY_SAMPLE_MS` (деф. 60000; `0` — выключен) берёт строки панели тем же кодом, что отвечает на `GET /api/vector-clearing/agent-positions` (`loadLiveAgentRows`), отбирает чистой функцией `pnlSamples` (`frontend/api/agent_pnl_history.js`) агентов с данными и пишет одним `INSERT … unnest` с `ON CONFLICT DO NOTHING`. Агент без значений R, U и выгоды в выборку не попадает: «нет данных» остаётся отсутствием строки, а не нулём. Строки агентов, помеченные как устаревшие (`stale`) или без позиции (`inactive`), по R и U не пишутся. Выгоду копит отдельный опрос снимка клиринга (`CE_SURPLUS_POLL_MS`, деф. 700 мс, модуль `frontend/api/ce_surplus_accum.js`), по одному разу на каждый такт (`batchId`); на каждом проходе записи копилка сбрасывается, а приращения пишутся строками с тем же `ts` (при совпадении `(agent_id, ts)` приращения складываются, R и U не перезаписываются). Сбой записи только логируется, ответы панели не страдают. Таблицу семплер создаёт лениво, при первом успешном проходе, поэтому BFF, стартовавший раньше PostgreSQL, начинает писать сам, без рестарта. После сброса статистики сделок кэши панели обнуляются, и в историю не попадает смешанная точка.
- **Reader: frontend-api**, `GET /api/vector-clearing/agent-pnl-history` и поля панели (контракт — `docs/06-api/rest/agent-pnl-history.md`).
- Хранение: раз в час BFF удаляет строки старше `PNL_HISTORY_RETENTION_DAYS` (деф. 7). Накопленная с сброса выгода в панели считается суммой приращений за доступный срок хранения.

## PostgreSQL (OLTP)

```sql
CREATE TABLE IF NOT EXISTS ce_agent_pnl_history (
  agent_id        TEXT NOT NULL,
  ts              TIMESTAMPTZ NOT NULL,
  realized_usdt   NUMERIC(38, 12),
  unrealized_usdt NUMERIC(38, 12),
  trade_fills     INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (agent_id, ts)
);
ALTER TABLE ce_agent_pnl_history ADD COLUMN IF NOT EXISTS surplus_inc_usdt NUMERIC(38, 12);
ALTER TABLE ce_agent_pnl_history ADD COLUMN IF NOT EXISTS surplus_tacts INTEGER;
ALTER TABLE ce_agent_pnl_history ADD COLUMN IF NOT EXISTS surplus_tacts_expected INTEGER;
```

| Поле | Смысл | Единицы |
| --- | --- | --- |
| `agent_id` | идентификатор агента (`T_BTC_binance`, `A_ETH_binanceokx`) | текст |
| `ts` | момент выборки, общий для всех агентов одного прохода | время |
| `realized_usdt` | R на момент `ts`; `NULL` — нет внешних сделок или данные не определены | USDT |
| `unrealized_usdt` | U на момент `ts`; `NULL` — не определён | USDT |
| `trade_fills` | сколько состоявшихся сделок учтено на момент `ts` | штуки |
| `surplus_inc_usdt` | выгода участника в клиринге за интервал с прошлой записи (сумма $S$ по учтённым тактам); `NULL` — потока не было | USDT |
| `surplus_tacts` | сколько тактов клиринга учтено в интервале (общее для всех участников: полнота опроса) | штуки |
| `surplus_tacts_expected` | сколько тактов было в интервале по шагу `batchId`; доля учтённых = `surplus_tacts / surplus_tacts_expected` | штуки |

Первичный ключ `(agent_id, ts)` служит индексом для чтения «агент за окно»; индекс `idx_ce_pnl_hist_ts` по `ts` обслуживает сумму выгоды с момента сброса статистики по всем агентам и часовую очистку.

## Как читать ряд

- R и U — накопительные величины с момента сброса статистики сделок (`trade_stats_reset_at`). После сброса ряд падает к нулю: это не ошибка, а начало нового отсчёта.
- Выгода хранится приращениями; на графике она накапливается внутри выбранного окна.
- История начинается с момента первого запуска семплера; прошлое не восстанавливается.
- U меняется и без новых сделок (оценка по текущим ценам), поэтому график R + U колеблется, а R меняется только при сделках.
- Выгода — оценка относительно кривой участника, а не деньги в кассе; не складывать с R и U.

## Миграция

Таблицу и три новых колонки создаёт сам BFF при старте (`CREATE TABLE IF NOT EXISTS`, `ADD COLUMN IF NOT EXISTS`); они же лежат в `infra/postgres/init.sql`. Для существующего тома dev отдельная миграция не нужна. Индекс `idx_child_orders_created` по `child_orders(created_at)` (запрос панели «заявки агентов за 2 часа») на существующем томе создают вручную без блокировки записи: `CREATE INDEX CONCURRENTLY IF NOT EXISTS idx_child_orders_created ON child_orders (created_at);`.

## Тип денег

Колонки — `NUMERIC`, а не `double`, чтобы история не нарушала правило «деньги не в `double` в persistence» (CLAUDE.md §1). Сами значения приходят в BFF числами и при записи приводятся к `numeric`; это отображаемая метрика, а не проводка.
