---
id: DOC-DATA-VENUE-LIQUIDITY-STATS
phase: 07-data
status: implemented (шаги 0–1 ADR-082, 2026-10-20); читатель market_data выключен флагом CE_DEPTH_FILL_COEF
level: sea
owner: core-team
source:
  - docs/03-architecture/adr/ADR-082-venue-liquidity-measurement-and-feedback.md
related:
  - docs/03-architecture/adr/ADR-060-venue-sim-fill-vs-real-trades.md (симулятор исполнения по ленте публичных сделок)
  - docs/04-domain/calculations/CALC-VENUE-LIQUIDITY-STATS.md (формулы оборота ленты и коэффициента исполнения)
  - docs/07-data/execution-reports.md
---

# Data: venue_liquidity_stats (измерение ликвидности внешних бирж)

## Назначение

Для каждой пары каждой внешней биржи хранит две оценки: **оборот ленты `ν`** (сколько реально торгуется в публичных сделках) и **коэффициент исполнения `f`** (какую долю объёма наших хедж-заявок биржа исполнила). Это основа для шагов 1–3 ADR-082 (глубина агента, размер заявки, порог хеджа); сейчас таблицу читает только экран «Внешние биржи».

## Owner

- **Writer и затравка: venues.** Адаптер биржи (`CexWsRestAdapter`) ведёт оценки в памяти (оценщики `cpp/venues/src/domain/liquidity_stats.hpp`), поток `PostgresLiquidityStatsRepository` раз в `CE_LIQ_STATS_PUBLISH_MS` (деф. 10000) берёт снимок через `VenuesLoop::CollectLiquidityStats()` и пишет UPSERT одной транзакцией только по парам с новым измерением. Ошибки записи только логируются (измерение не критично для торговли), соединение пересоздаётся.
- **Reader: frontend-api** (`GET /api/venues/liquidity-stats`, модуль `venue_liquidity_view.js`); **market_data** (`PgVenueLiquidityReader`, фоновый поток, читает `fill_coef` и `fill_orders_eff` строк с `fill_orders > 0` не старше `CE_DEPTH_FILL_MAX_AGE_S`) — только при `CE_DEPTH_FILL_COEF=1`; позже — `ledger` за флагом ADR-082.

## PostgreSQL (OLTP)

```sql
CREATE TABLE IF NOT EXISTS venue_liquidity_stats (
  venue            TEXT NOT NULL,
  symbol           TEXT NOT NULL,
  tape_quote_per_s DOUBLE PRECISION NOT NULL DEFAULT 0,
  tape_samples     BIGINT NOT NULL DEFAULT 0,
  fill_coef        DOUBLE PRECISION NOT NULL DEFAULT 1,
  fill_weight      DOUBLE PRECISION NOT NULL DEFAULT 0,
  fill_orders      BIGINT NOT NULL DEFAULT 0,
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (venue, symbol)
);
ALTER TABLE venue_liquidity_stats ADD COLUMN IF NOT EXISTS fill_orders_eff DOUBLE PRECISION NOT NULL DEFAULT 0;
```

| Поле | Смысл | Единицы |
| --- | --- | --- |
| `venue` | идентификатор биржи (`VenueAdapter::VenueId`) | текст |
| `symbol` | канонический ключ ленты (`BTCUSDT`, `ETHBTC`; без разделителей, верхний регистр) | текст |
| `tape_quote_per_s` | оборот публичных сделок, скользящее среднее (постоянная `CE_TAPE_EWMA_TAU_S`, деф. 900 с) | валюта котирования пары в секунду |
| `tape_samples` | сколько чтений ленты вошло в оценку; 0 — оценки ещё нет | штуки |
| `fill_coef` | `f` ∈ [0, 1]: исполнено / запрошено по хедж-заявкам (постоянная забывания `CE_FILL_COEF_TAU_S`, деф. 1800 с); нет заявок ⇒ 1 | доля |
| `fill_weight` | вес оценки `f`: запрошенный объём заявок с забыванием, приведённый к моменту записи | базовая валюта пары |
| `fill_orders` | сколько заявок вошло в `f` (накопительно с запуска `venues`); 0 — `f` не измерен, экран показывает «—» | штуки |
| `fill_orders_eff` | число заявок в `f` с забыванием на момент записи: убывает как exp(−Δt/τ) без новых заявок; вес доверия к `f` у потребителя (шаг 1 ADR-082) | штуки |
| `updated_at` | момент, когда измерение пары менялось в последний раз (пара пишется, только если прибавились чтения ленты или заявки); экран помечает строки старше 120 с как неактуальные | время |

Числа `double` — метрики наблюдаемости, не деньги (CLAUDE.md §1.1); в проводки `ledger` они не попадают. Потребитель денежного пути (шаг 2 ADR-082) квантует вычисленное ограничение размера в `Decimal` на своей границе.

## Миграция

`CREATE TABLE IF NOT EXISTS` и `ALTER TABLE ... ADD COLUMN IF NOT EXISTS fill_orders_eff` выполняются при старте `venues` (`PostgresLiquidityStatsRepository::EnsureSchema`) и лежат в `infra/postgres/init.sql`. Для существующего тома dev отдельная миграция не нужна: таблица и колонка создаются первым стартом новой версии `venues`. Выкладывать сначала `venues`, затем `market_data`: до появления колонки читатель получает ошибку, пишет предупреждение и работает с множителем 1.

## Жизненный цикл и ограничения

- При перезапуске `venues` читает все строки таблицы (`LoadAll`) и затравливает оценки: `f`, вес и число заявок с забыванием приводятся к моменту старта забыванием за время простоя, `ν` берётся не старше часа (`CE_TAPE_SEED_MAX_AGE_S`). Затравленные пары не перезаписываются, пока по ним не прибавились чтения ленты или заявки, поэтому `updated_at` остаётся временем последнего настоящего измерения. Затравка применяется при первом чтении ленты пары и только к нетронутым оценкам; первое чтение ленты выборки `ν` по-прежнему не даёт. Строка в базе остаётся с прежними значениями, пока `venues` не запишет новую для той же пары.
- Строки удаляются только вручную (удалённая биржа или пара остаётся в таблице; экран показывает её как неактуальную по `updated_at`).
- Объём таблицы ограничен числом пар (десятки строк), хранение и ретеншн не требуются.
- Выдача ленты ограничена 100 последними сделками. Если все 100 новые (так у Binance и OKX на ликвидных парах), `ν` считается по охваченному интервалу биржевого времени, а не по времени между чтениями; без биржевого времени (Coinbase) такая выборка пропускается.
