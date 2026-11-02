---
id: DOC-DATA-CE-PNL-TOTALS-HISTORY
phase: 07-data
status: implemented (2026-10-28, CR-F18-005)
level: sea
owner: core-team
source:
  - docs/02-system/features/F-18-ce-capital-position-hedge/feature.yaml (AC-F18v2-31)
related:
  - docs/03-architecture/adr/ADR-087-ce-agent-pnl-internal-external-total.md (определения R, U и итогов)
  - docs/04-domain/calculations/CALC-CE-AGENT-PNL.md (формулы)
  - docs/06-api/rest/agent-pnl-history.md (маршрут `GET /api/vector-clearing/pnl-history`, §3)
  - docs/07-data/ce-agent-pnl-history.md (история отдельных агентов)
---

# Data: ce_pnl_totals_history (история итогов заработка агентов по охватам)

## Назначение

Хранит, как менялись во времени итоги заработка агентов CE, посчитанные в ledger (`GetAgentPnl`: итоги «все агенты» и по видам), чтобы показать график «Заработок во времени» на вкладке «PnL». Запись — раз в минуту, по строке на охват: все агенты (`all`), переводчики (`translator`), арбитражёры (`arbitrageur`). Это журнал значений ledger, а не второй расчёт: BFF переписывает числа итогов как есть и ничего не складывает (сумму «внутренняя + внешние» для группы «итого» считает при чтении `buildTotalsHistoryDto`, для отображения).

Таблица только для отображения: ledger, risk и matching её не читают, в проводки она не попадает.

## Owner

- **Writer: frontend-api (BFF).** Семплер `samplePnlHistoryOnce` (`frontend/api/server.js`) в том же проходе, что пишет `ce_agent_pnl_history`, берёт итоги из `loadLiveAgentRows` (`pnlTotals`, результат ledger) и вставляет до трёх строк `INSERT … ON CONFLICT (scope, ts) DO NOTHING` (чистая функция выборки `totalsSamples`, `frontend/api/agent_pnl_history.js`). Таблицу создаёт `ensurePnlHistoryTable` (первый проход семплера или первое чтение графика). Ledger недоступен или нет ни одного значения — строк нет, на графике разрыв, нули не пишутся.
- **Reader: frontend-api**, `GET /api/vector-clearing/pnl-history` (контракт — [agent-pnl-history](../06-api/rest/agent-pnl-history.md) §3).
- Хранение: раз в час BFF удаляет строки старше `PNL_HISTORY_RETENTION_DAYS` (деф. 7).

## PostgreSQL (OLTP)

```sql
CREATE TABLE IF NOT EXISTS ce_pnl_totals_history (
  scope        TEXT NOT NULL CHECK (scope IN ('all', 'translator', 'arbitrageur')),
  ts           TIMESTAMPTZ NOT NULL,
  internal_r   NUMERIC(38, 12),
  internal_u   NUMERIC(38, 12),
  external_r   NUMERIC(38, 12),
  external_u   NUMERIC(38, 12),
  agents       INTEGER NOT NULL DEFAULT 0,
  incomplete   INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (scope, ts)
);
```

| Поле | Смысл | Единицы |
| --- | --- | --- |
| `scope` | охват итога: `all`, `translator`, `arbitrageur` | текст |
| `ts` | момент выборки, общий для строк одного прохода (совпадает с `ts` в `ce_agent_pnl_history`) | время |
| `internal_r` | R внутренней биржи: сумма выгод сделок к справедливой цене по агентам охвата с известным R; `NULL` — не определён | USDT |
| `internal_u` | U внутренней биржи (ledger отдаёт 0: на внутренней бирже всё реализуется в такте); `NULL` — не определён | USDT |
| `external_r` | R внешних бирж по агентам охвата с известным R; `NULL` — не определён | USDT |
| `external_u` | U внешних бирж по агентам охвата с известным U; `NULL` — не определён | USDT |
| `agents` | сколько агентов охвата в итоге ledger | штуки |
| `incomplete` | у скольких из них R или U неизвестны (после перезапуска ledger, до первой цены) | штуки |

Первичный ключ `(scope, ts)` служит индексом для чтения «охват за окно»; отдельный индекс по `ts` не нужен (чистка идёт по `ts < …` над таблицей из ~3 строк в минуту, 4 320 строк в сутки, 30 тыс. за срок хранения — полный просмотр дёшев).

## Как читать ряд

- R и U в итогах — накопительные величины ledger с момента внедрения модели C′ (ADR-087) или последнего сброса позиций. После сброса позиций ряд падает к нулю: это начало нового отсчёта.
- Сумма `internal_r` по всем агентам должна быть близка к нулю (инвариант внутренней биржи): ряд `all / internal` — прямая проверка учёта на графике.
- Пока `incomplete > 0`, R и U суммированы по разным множествам агентов; после перезапуска ledger это временно, пока агенты не получат первую цену.
- История начинается с выкладки CR-F18-005; прежние значения по определениям модели C′ не восстанавливаются.

## Когда читается

`GET /api/vector-clearing/pnl-history?scope=…&hours=…` — выборка «охват за окно» по первичному ключу `(scope, ts)` без сортировки. Окно не больше `min(168, PNL_HISTORY_RETENTION_DAYS × 24)` часов.

## Миграция и откат

Таблицу создаёт BFF при первом проходе семплера или первом чтении графика (`ensurePnlHistoryReady`: один общий промис, чтобы параллельные вызовы не выполняли DDL одновременно) и `infra/postgres/init.sql` при создании тома. Откат — удаление таблицы командой `DROP TABLE` (она только для отображения; история восстановима лишь с момента повторной выкладки). См. [migrations](migrations.md).

## Used In

- Feature: F-18 (AC-F18v2-31, capability CAP-F18-AGENT-PNL).
- Контракт: [agent-pnl-history](../06-api/rest/agent-pnl-history.md) §3; экран — `UI-PNL-HISTORY` на вкладке «PnL».
