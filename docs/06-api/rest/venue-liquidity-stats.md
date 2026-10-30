---
id: DOC-API-VENUE-LIQUIDITY-STATS
phase: 06-api
status: implemented (ADR-082, шаг 0, 2026-10-20)
level: sea
owner: core-team
source:
  - docs/03-architecture/adr/ADR-082-venue-liquidity-measurement-and-feedback.md
related:
  - docs/07-data/venue-liquidity-stats.md (таблица, из которой читает маршрут)
  - docs/04-domain/calculations/CALC-VENUE-LIQUIDITY-STATS.md (формулы оборота ленты и коэффициента исполнения)
---

# REST: GET /api/venues/liquidity-stats

Маршрут BFF (`frontend/api/server.js`, обработчик `handleVenues`). Отдаёт измерения ликвидности внешних бирж по парам для панели «Ликвидность бирж» на экране «Внешние биржи». Данные читает из PostgreSQL `venue_liquidity_stats` (пишет `venues`) и только размечает (`frontend/api/venue_liquidity_view.js`): оборот ленты и коэффициент исполнения как есть, без пересчёта.

## Ответ 200

```json
{
  "venues": [
    {
      "venue": "binance",
      "pairs": [
        {
          "symbol": "BTCUSDT",
          "base": "BTC",
          "quote": "USDT",
          "has_tape": true,
          "tape_quote_per_s": 29000.0,
          "tape_quote_per_min": 1740000.0,
          "tape_samples": 75,
          "has_fill": true,
          "fill_coef": 0.81,
          "fill_weight": 3.16,
          "fill_orders": 113,
          "updated_at_ms": 1792510000000,
          "age_ms": 7000,
          "stale": false
        }
      ]
    }
  ],
  "generatedAt": "2026-10-20T14:19:05.351Z"
}
```

| Поле | Смысл |
| --- | --- |
| `symbol` | канонический ключ ленты (`BTCUSDT`, `ETHBTC`) |
| `base`, `quote` | разбор ключа по известным котировкам (`USDT`, `USDC`, `USD`, `EUR`, `BTC`, `ETH`); незнакомая котировка ⇒ `quote: null` |
| `has_tape` | есть ли оценка оборота ленты (`tape_samples > 0`); иначе `tape_quote_*` равны `null` |
| `tape_quote_per_s`, `tape_quote_per_min` | оборот публичных сделок в валюте котирования пары за секунду и за минуту |
| `tape_samples` | сколько чтений ленты вошло в оценку |
| `has_fill` | есть ли хотя бы одна учтённая хедж-заявка (`fill_orders > 0`); иначе `fill_coef` и `fill_weight` равны `null` |
| `fill_coef` | коэффициент исполнения $f\in[0,1]$ |
| `fill_weight` | вес оценки $f$: запрошенный объём заявок с забыванием, базовая валюта |
| `fill_orders` | сколько заявок вошло в $f$ |
| `updated_at_ms`, `age_ms` | когда измерение пары менялось в последний раз и сколько прошло |
| `stale` | запись старше 120 с или без `updated_at` |

Нет данных ⇒ `venues: []` (не ошибка). Число заявок с забыванием (`fill_orders_eff`) в ответ не входит: оно нужно `market_data` и отдельным полем на экране не показывается.

## Ошибки

| Код | Когда |
| --- | --- |
| 503 `postgres_unavailable` | не настроено подключение к PG |
| 502 `pg_error` | запрос к PG не удался (например, таблица ещё не создана первым стартом `venues`) |

Авторизации на маршруте нет, как и у остальных маршрутов чтения `/api/venues/*`.
