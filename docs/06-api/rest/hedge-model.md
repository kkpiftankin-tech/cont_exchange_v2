---
id: DOC-API-HEDGE-MODEL
phase: 06-api
status: implemented (2026-10-29; CR-F18-013, ADR-088)
level: sea
owner: core-team
source:
  - docs/02-system/features/F-18-ce-capital-position-hedge/feature.yaml (AC-F18v2-34, AC-F18v2-35)
related:
  - docs/06-api/grpc/ledger-agent-positions.md (источник: поле hedge_model)
  - docs/07-data/ce-agent-band-history.md (таблица, из которой читает маршрут §2)
  - docs/04-domain/calculations/CALC-CE-HEDGE-THRESHOLD-MODEL.md (формулы, единицы)
  - docs/03-architecture/adr/ADR-088-ce-hedge-threshold-model.md
---

# REST: модельные пороги хеджа в строках агентов и история порогов

Два изменения контракта BFF (`frontend/api/server.js`, модуль `hedge_model_dto.js`): поля строки панели «Живые позиции» (§1) и история порогов с выгрузкой для эксперта (§2). Оба — **аддитивные**: существующие поля не менялись. BFF ничего не пересчитывает: числа приходят из ledger (`GetAgentPositions`).

## 1. GET /api/vector-clearing/agent-positions — новые поля

В каждой строке `agentRows[]`:

| Поле | Тип | Смысл |
| --- | --- | --- |
| `z_lim`, `z_mkt` | число или `null` | рабочие пороги хеджа, тыс. USDT (по ним идут заявки; в режиме `on` — модельные). `null` — ledger не прислал. У связки — с той же ноги, что и `hmodel` (приёмник, затем источник) |
| `hmodel` | объект или `null` | модельный блок; `null` — режим `off` или копия proto в BFF не знает поле (см. `hmodel_supported`) |

`hmodel`: `{ mode, computed, applied, reason, regime, b_m, b_t, sz_est, mu_est, theta_est, peclet, sigma_bps, fee_maker_bps, fee_taker_bps, transfer_threshold, transfer_target, est_minutes, params_hash, computed_at_ms, age_ms }`.

**Потребитель на экране «Клиринг».** Блок «Пороги хеджа: действующие и по модели» (`frontend/web/src/pages/VectorClearing/HedgeModelThresholds.js`, AC-F18v2-37) читает из строки `z_lim`, `z_mkt`, `c_total`, `agent_kind` и `hmodel.{mode, computed, applied, reason, regime, b_m, b_t, transfer_threshold, transfer_target, sz_est, est_minutes}`. Браузер ничего не считает: ноль показывается нулём (цель перевода 0), `null` — прочерком; без `hmodel` ни у одной строки показана одна строка-пояснение (режим `off`).

- `mode` — `shadow` или `on`; `applied=true` только в `on`.
- `computed=false` — модель не посчитана, `reason` называет причину (`no_sigma`, `sigma_samples`, `sigma_stale`, `invalid_input`, `budget`, `numeraire`); `b_m`, `b_t`, `regime`, `peclet`, `sigma_bps`, комиссии и пороги перевода при этом `null`.
- `regime` — `noise` | `mixed` | `drift` (только у переводчика); неизвестное значение → `null`.
- **Ноль — законное значение, отличать от «нет»:** `b_m=0` (ребейт мейкера), `transfer_target=0` (чисто фиксированная плата сети) приходят числом `0`; отсутствие — `null`.
- `est_minutes=0` значит, что `sz_est` — значение по умолчанию, а не оценка по данным.
- `peclet` не конечный → `null`; `age_ms` — возраст расчёта (считает BFF).

Корень ответа: `hmodel_supported` — `true`, если копия proto в BFF знает `AgentPosition.hedge_model`; `false` — копия устарела (`sh frontend/api/sync-proto.sh` и пересборка), `null` — клиент ledger ещё не создан. Без этого признака потеря поля неотличима от режима `off`.

## 2. GET /api/vector-clearing/hedge-model-history

История порогов, записанная минутным проходом BFF.

### Параметры запроса

| Параметр | По умолчанию | Смысл |
| --- | --- | --- |
| `agent_id` | все агенты | идентификатор агента |
| `hours` | 24 | окно, не более срока хранения (`BAND_HISTORY_RETENTION_DAYS` × 24, деф. 336) |
| `limit` | 20000 | число строк, не более 50000; при переполнении возвращаются самые свежие строки (порядок по времени сохраняется), ответ помечается `truncated: true` (csv — заголовок `X-Truncated: 1`) |
| `format` | `json` | `csv` — выгрузка для эксперта (`Content-Disposition: attachment; hedge-model-history.csv`) |

### Ответ (json)

`{ window_hours, agent_id, columns, rows[], count, limit, truncated, generatedAt }`. Колонки: `ts, agent_id, kind, mode, position, in_flight, z_lim, z_mkt, b_m, b_t, regime, computed, applied, reason, sz_est, mu_est, theta_est, peclet, sigma_bps, fee_maker_bps, fee_taker_bps, transfer_threshold, transfer_target, est_minutes, params_hash`. Единицы и смысл — `docs/07-data/ce-agent-band-history.md`.

### Ошибки

`503 postgres_unavailable`, `502 pg_error`. Пустая таблица — `rows: []`, не ошибка. Маршрут только читает; авторизации нет (как у соседних маршрутов чтения), записи в журнал оператора не требуется.
