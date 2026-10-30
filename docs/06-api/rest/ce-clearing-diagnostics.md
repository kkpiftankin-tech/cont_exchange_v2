---
id: DOC-API-REST-CE-CLEARING-DIAG
phase: 06-api
status: draft
owner: core-team
source:
  - ADR-079
related:
  - docs/03-architecture/adr/ADR-079-ce-clearing-decision-diagnostics.md
  - docs/04-domain/calculations/CALC-CE-CLEARING-DIAG.md
  - cpp/matching/src/main.cpp
  - frontend/api/ce_clearing_view.js
---

# REST API: диагностика решения CE-клиринга (ADR-079)

Два read-only эндпоинта. Они ничего не меняют и не влияют на клиринг; auth — как у остальных служебных портов (в MVP отсутствует, см. F-16 / F-01).

## `GET /ce/clearing/last` (matching, порт метрик `MATCHING_HTTP_ADDR`, по умолчанию 8081)

Снимок ПОСЛЕДНЕГО решения CE-клиринга. Хранится только последний такт (в памяти сервиса).

| Код | Тело | Когда |
| --- | --- | --- |
| 200 | JSON снимка | клиринг уже считался |
| 404 | `{"error":"no_snapshot"}` | после старта ещё не было такта |

Поля JSON (числа — `double`, потенциалы и полки в промилле, потоки в тыс. USDT; нефинитные значения — `null`):

| Поле | Смысл |
| --- | --- |
| `batch_id`, `event_time_ms`, `solved_at_ms`, `numeraire` | такт и момент решения |
| `converged`, `iters`, `max_imbalance` | итог решателя |
| `quotes_received` | сколько котировок пришло в `ce.clearing.input` |
| `nodes[]` | `id`, `asset`, `venue`, `potential_pm`, `pinned` |
| `edges[]` | `name`, `kind` (`translator`/`arbitrageur`), `anchor_pm`, `anchor_raw_pm`, `skew_pm`, `potential_gap_pm`, `deviation_pm`, `dead_zone_pm` + состав `dz_age_pm`/`dz_spread_pm`/`dz_commission_pm`, `age_ms`, `sigma_pm`, `margin_pm`, `depth`, `flow_kusdt`, `state` (`active`/`dead_zone`/`no_depth`/`invalid`) |
| `cycles[]` | типовые циклы «актив × пара бирж через USDT»: `drive_pm` (G), `threshold_pm` (Σc), `margin_pm`, `state` (`open`/`blocked`/`no_depth`), `parts[]` |
| `dropped[]` | отброшенные котировки: `asset`, `venue`, `quote`, `reason` (`stale`/`thin_depth`/`invalid_depth`), `age_ms`, `depth`, `threshold` |
| `summary` | `edges`, `active`, `active_translators`, `active_arbitrageurs`, `in_dead_zone`, `no_depth`, `invalid`, `nearest_edge`, `nearest_margin_pm`, `cycles_total`, `cycles_open`, `min_cycle`, `min_cycle_margin_pm` |

Формулы — [CALC-CE-CLEARING-DIAG](../../04-domain/calculations/CALC-CE-CLEARING-DIAG.md).

## `GET /api/vector-clearing/ce-clearing` (BFF `frontend/api`, порт 8090)

Собирает ответ из `GET {MATCHING_ADDR}/ce/clearing/last` и админ-API `venues` (`GET {VENUES_ADDR}/api/v1/venues`), оба запроса параллельно с таймаутом 3 с. Недоступность одного источника не ломает ответ: нет снимка — `available=false`, `solve=null`, `error` и объяснение; нет админ-API — список бирж строится по снимку без статусов опроса.

| Поле ответа | Смысл |
| --- | --- |
| `available`, `error` | есть ли снимок, причина отсутствия (`http_404`, таймаут и т. п.) |
| `solve` | снимок с добавками: `ageMs` (возраст решения), `*_bp` (базисные пункты), у циклов — составляющие порога `thr_age_pm`/`thr_spread_pm`/`thr_commission_pm`/`thr_transfer_pm` |
| `explanation` | `level` (`ok`/`warn`/`error`), `headline`, `bullets[]` — объяснение словами; решение старше 30 с объявляется устаревшим |
| `liquidity.venues[]` | по биржам: статус, предохранитель, режим и период опроса; по парам: в клиринге или отброшена и почему, возраст кривой и опроса, глубина, полка, σ |
| `liquidity.dropped[]` | как `dropped` снимка |
| `generatedAt` | время сборки ответа |

Потребитель — панель `CeClearingPanel` на вкладке «Граф» (`/ce-graph-live`), опрос раз в 3 с.
