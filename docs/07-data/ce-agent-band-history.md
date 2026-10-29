---
id: DOC-DATA-CE-AGENT-BAND-HISTORY
phase: 07-data
status: implemented (2026-10-29; CR-F18-013, ADR-088)
level: sea
owner: core-team
source:
  - docs/02-system/features/F-18-ce-capital-position-hedge/feature.yaml (AC-F18v2-34, AC-F18v2-35)
related:
  - docs/03-architecture/adr/ADR-088-ce-hedge-threshold-model.md (решение и условия включения рабочего режима)
  - docs/04-domain/calculations/CALC-CE-HEDGE-THRESHOLD-MODEL.md (что такое b_m, b_t, режим, оценки потока)
  - docs/06-api/grpc/ledger-agent-positions.md (поле hedge_model, из которого берётся запись)
  - docs/06-api/rest/hedge-model.md (маршрут чтения и выгрузки)
  - docs/07-data/ce-agent-pnl-history.md (образец: та же схема записи BFF-сэмплером)
---

# Data: ce_agent_band_history (история порогов хеджа агентов CE)

## Назначение

Хранит, как менялись во времени рабочие и модельные пороги хеджа каждого агента, его позиция и оценки потока. Нужна для двух вещей:

- **данные для эксперта по трейдингу:** сравнить пороги правила проекта и модели во времени, увидеть режим рынка (шум, смешанный, дрейф), оценки шума потока и дрейфа, число Пекле, версию настроек модели; выгрузка в CSV (`docs/06-api/rest/hedge-model.md`);
- **закрыть дыру «истории позиций агентов нет»** (`docs/00-methodology/known-gaps.md`): позиция $c$ и $c+\text{in-flight}$ раз в минуту.

Таблица только для отображения и анализа: ledger, risk и matching её не читают, в проводки она не попадает.

## Owner

- **Writer: frontend-api (BFF)** — тот же минутный проход, что пишет `ce_agent_pnl_history` (`samplePnlHistoryOnce` → `writeBandHistory`). Пишутся те же числа, что видит панель (ответ ledger `GetAgentPositions`, поле `hedge_model`); BFF ничего не пересчитывает. Таблицу создаёт BFF при первом проходе (`CREATE TABLE IF NOT EXISTS`). Если `PNL_HISTORY_SAMPLE_MS=0`, история порогов тоже не пишется.
- **Reader: frontend-api (BFF)** — маршрут `/api/vector-clearing/hedge-model-history`.

## PostgreSQL (OLTP)

```sql
CREATE TABLE IF NOT EXISTS ce_agent_band_history (
  agent_id TEXT NOT NULL, ts TIMESTAMPTZ NOT NULL,            -- ts = начало минуты (floor(now/60 с)), не now()
  kind TEXT, mode TEXT,                                       -- 'translator' | 'arbitrageur'; 'shadow' | 'on'
  position NUMERIC(38,12), in_flight NUMERIC(38,12),          -- c и in-flight, тыс. USDT, знаковые
  z_lim NUMERIC(38,12), z_mkt NUMERIC(38,12),                 -- РАБОЧИЕ пороги (по ним идут заявки)
  b_m NUMERIC(38,12), b_t NUMERIC(38,12),                     -- пороги МОДЕЛИ; NULL — не посчитаны; 0 — законное значение
  regime TEXT, computed BOOLEAN NOT NULL DEFAULT FALSE, applied BOOLEAN NOT NULL DEFAULT FALSE, reason TEXT,
  sz_est DOUBLE PRECISION, mu_est DOUBLE PRECISION, theta_est DOUBLE PRECISION, peclet DOUBLE PRECISION,
  sigma_bps DOUBLE PRECISION, fee_maker_bps DOUBLE PRECISION, fee_taker_bps DOUBLE PRECISION,
  transfer_threshold NUMERIC(38,12), transfer_target NUMERIC(38,12),   -- связка: B и b
  est_minutes INTEGER NOT NULL DEFAULT 0, params_hash BIGINT,
  PRIMARY KEY (agent_id, ts));
CREATE INDEX IF NOT EXISTS idx_ce_band_hist_ts ON ce_agent_band_history (ts);
```

| Поле | Смысл | Единицы |
| --- | --- | --- |
| `position`, `in_flight` | неотправленный остаток $c$ и отправленное в заявки | тыс. USDT |
| `z_lim`, `z_mkt` | рабочие пороги (правило проекта или, в режиме `on`, модель) | тыс. USDT |
| `b_m`, `b_t` | пороги модели после пола и потолка | тыс. USDT |
| `regime` | `noise` / `mixed` / `drift` по числу Пекле | — |
| `applied` | режим `on`: пороги модели использованы для заявок | — |
| `sz_est`, `mu_est`, `theta_est` | шум потока, дрейф (знаковый, сырая оценка до отсечки погрешности $2\,\mathrm{SE}$; в расчёт идёт отсечённая — видна по `peclet` и `regime`), скорость возврата (диагностика) | тыс. USDT/√мин; тыс. USDT/мин; 1/мин |
| `peclet` | число Пекле на пороге тейкера | — |
| `sigma_bps` | волатильность курса пары | bps за √мин |
| `fee_maker_bps`, `fee_taker_bps` | комиссии (ADR-089, без спреда) | bps |
| `transfer_threshold`, `transfer_target` | порог и цель перевода связки | тыс. USDT |
| `est_minutes` | минут накоплено для оценки; **0 ⇒ `sz_est` — значение по умолчанию, а не оценка** | мин |
| `params_hash` | версия набора настроек модели (γ, κ, a, s/2, θ, K, …) | — |

## Как читать ряд

- Одна строка на агента и минуту; пропуск минуты — разрыв ряда (BFF, ledger или стенд были недоступны), не нулевая позиция.
- `computed=false` и `reason` — модель не посчитана (нет свежей волатильности и т. п.); рабочие пороги при этом всё равно записаны.
- Рестарт ledger и сброс позиций обнуляют оценки потока (`est_minutes` → 0) — не сравнивать `sz_est` до и после без этого признака.
- Выжимки для эксперта берутся из этой таблицы вместе с версией настроек `params_hash`; при смене настроек ряд разделять по хешу.

## Миграция

Новых действий на существующем томе не нужно: таблицу создаёт BFF (`CREATE TABLE IF NOT EXISTS`). Откат — `CE_HMODEL_MODE=off` (записи прекращаются) или `DROP TABLE IF EXISTS ce_agent_band_history;` (внешних ключей, представлений и триггеров нет, старые версии таблицу не знают).

## Нагрузка и хранение

~100 агентов × 1 строка в минуту ≈ 144 тыс. строк в сутки, ≈ 0,3 КБ на строку с индексом (≈ 45 МБ в сутки). Хранение `BAND_HISTORY_RETENTION_DAYS` (деф. 14 суток, ≈ 0,6 ГБ), очистка раз в час обычным `DELETE` по индексу `ts` (так же, как у остальных `*_history`). Запись идемпотентна: `ON CONFLICT (agent_id, ts) DO NOTHING`.

## Тип денег

Позиции, пороги и пороги перевода — `NUMERIC` (значения ledger, `Decimal` масштаба 6); оценки потока, число Пекле, волатильность и ставки — `DOUBLE PRECISION` (диагностика модели, в проводки и заявки не идёт; в модели `double` разрешён только для порогов и диагностик, CLAUDE.md §1.1).
