# Implementation Tasks: F-18 D1+#7 — динамический порог band `Γ=γσ²τ` (per-asset волатильность)

> **Тип изменения:** `MVP implementation` (matching/risk-политика, ADR-066 D1). Кросс-компонент:
> market_data (σ) → PG → ledger (порог). **Основание:** [ADR-066](../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md)
> (Γ=ρ=γσ²τ, дизайн D1), Кривые §6.1 (`ρ≈γσ²τ`) / §6.4 (`Z̄=clim/Γ`). Закрывает status.md §3 #2 (реализация) и #7.
> **Дата:** 2026-09-28. **Ветка:** `feat/F-18-ce-treasury-nop`.

## Цель

Порог band выводится из единого per-asset `Γ = γ·σ²·τ` (§6.4 `z_lim=clim/Γ`, `z_mkt=cmkt/Γ`),
масштабируясь по волатильности актива, вместо плоского `ce_band_fee_k`. За флагом
`CE_BAND_GAMMA_MODE` (0=плоский D2 дефолт, 1=динамический D1), с fallback на `ce_band_fee_k` при
недоступной σ.

## Задачи

### T-F18-701 — Схема `ce_asset_volatility` (данные)
**Приоритет:** Высокий. **Файлы:** `docs/07-data/` (новый doc), `infra/postgres/init.sql`.
- Таблица `ce_asset_volatility(asset TEXT PK, venue TEXT, sigma NUMERIC, window_sec INT, samples INT, updated_at TIMESTAMPTZ)`.
  Первый срез — per-asset (venue опц., '' = агрегат). Owner writer: market_data; reader: ledger.
- Контракт-doc: смысл σ (EWMA реализованной волатильности лог-доходностей mid), единицы (доля за τ),
  staleness, кто пишет/читает.
- **Приёмка:** clean apply + re-apply идемпотентны; doc в `docs/07-data/` слинкован с ADR-066.

### T-F18-702 — EWMA-оценка σ в market_data + PG-writer
**Приоритет:** Высокий. **Файлы:** `cpp/market_data/src/...` (app + infra PG repo).
- Держать per-(venue,asset) EWMA лог-доходностей mid-цены; σ = sqrt(EWMA var). Окно/λ — env
  (`CE_VOL_EWMA_HALFLIFE_SEC`, дефолт ~60с). Писать в `ce_asset_volatility` неблокирующе (паттерн
  fill_diagnostics: очередь+воркер, переиспользуемое соединение — см. T-F18-601/602).
- **Приёмка:** свежие строки σ по активным активам; σ растёт на волатильном участке, падает на спокойном.

### T-F18-703 — Ledger: Γ=γσ²τ, z_lim=clim/Γ за флагом + калибровка
**Приоритет:** Высокий. **Файлы:** `cpp/ledger/src/app/ledger_uc.{cpp,hpp}`.
- `CE_BAND_GAMMA_MODE` (0=D2 плоский `ce_band_fee_k` дефолт; 1=D1). При D1: читать σ per-asset
  (TTL-кэш, паттерн `LoadBandFeeConfig`), `Γ = γ·σ²·τ` (γ=`gamma` из f05a, τ=`CE_BAND_TAU_SEC`
  деф=`CE_IMPACT_TAU_SEC`), `z_lim=clim/Γ`, `z_mkt=cmkt/Γ`. rt-множитель арбитражёра сохранить.
- Калибровочная константа `Γ0` (env `CE_BAND_GAMMA0`) так, что при σ_ref порог ≈ текущий ~18@10bps.
- Fallback на `ce_band_fee_k` при отсутствии/устаревании σ. Логировать источник (dynamic/flat).
- **Приёмка:** unit-тест — при σ_ref порог совпадает с прежним (±ε); при 2×σ порог растёт ~4× (σ²).

### T-F18-704 — Наблюдаемость (σ и итоговые пороги)
✅ **СДЕЛАНО (2026-09-28, коммит `e4d04efc`).** BFF agent-detail отдаёт `volatility{sigma,samples,
ageMs,sigmaRef,gammaCap,scale,zLimDynamic,zMktDynamic}` (σ из `ce_asset_volatility`, scale = формула
ledger `VolScaleFactor` — превью «при CE_BAND_GAMMA_MODE=1»). Drill-down: панель «Волатильность σ →
Γ=γσ²τ» (σ, выборок, свежесть, множитель, динамические vs плоские пороги). σ реальный из БД, не
фейк. Проверено: T_BTC_okx σ=4.76e-4, scale=1.10×, bundle main.1b5b47d4.js.
Остаток (с включением флага, 705): режим (D1/D2) и АКТИВНЫЕ пороги брать из ledger по DTO (сейчас
превью считается в BFF по σ; при flip ledger должен экспонировать эффективный порог) + WARN-лог
промаха σ в ledger.
**Приоритет:** Средний. **Файлы:** BFF `frontend/api/server.js`, drill-down.

### T-F18-705 — Проверка масштабирования + калибровка
**Приоритет:** Высокий. **Файлы:** `Testing/` probe или расширение существующего.
- На dev с `CE_BAND_GAMMA_MODE=1`: пороги активов различаются по волатильности; при σ_ref совпадают с
  D2; позиции сходятся к band; lag 0 (throughput не задет).
- **Приёмка:** высоковолатильный актив имеет больший порог, чем спокойный; калибровка при σ_ref держит прежний ~18.

**Пререквизиты включения флага (из code-review + 3 профильных ревью T-F18-703, 2026-09-28):**
- **Unit-тест** ветки динамического масштаба (по образцу `ce_band_hedge_a7_test.cpp`): flag-off
  байт-в-байт; fallback при stale/low-samples/missing σ; clamp ИТОГОВОГО `scale` на границах [0.1,10]
  (не только `ratio` — γ-канал тоже, domain-review HIGH); `z_mkt ≥ z_lim` после масштаба.
- **Калибровка σ_ref (domain-review, БЛОКЕР дифференциации):** дефолт `CE_BAND_SIGMA_REF=0.0005` выше
  наблюдаемой σ спокойных активов (BTC per-sec ≈ 1.4e-4). При нём `ratio=(σ_ref²/σ²)` для спокойных
  массово упрётся в потолок 10× ⇒ плоское 10-кратное расширение вместо per-asset дифференциации —
  цель #7 НЕ достигается. Перед flip: снять реальное распределение σ по активам на dev и выставить
  σ_ref (и, возможно, границы clamp) так, чтобы активы реально различались, а не липли к потолку.
- **T-F18-706 (perf + owner-gate):** (а) `LoadAssetSigma` открывает `pqxx::connection` под `mu_` (как
  `LoadBandFeeConfig`) — перед включением перевести на переиспользуемое соединение/async (паттерн
  T-F18-601/602), чтобы блокирующий PG-I/O не держал `mu_` всего батча; (б) **явный owner sign-off
  калибровки `σ_ref`/`Γ0` перед flip `CE_BAND_GAMMA_MODE=1`** в общем dev/staging (arch-review §4;
  прецедент ручной рассинхронизации `f05a_clearing_config` после рестарта — память проекта).

## Порядок

701 (схема) → 702 (σ writer) → 703 (ledger reader+Γ, флаг off по умолчанию) → 704 (обзор) →
705 (включить флаг на dev, калибровать, проверить). Флаг off до 705 — прод-поведение не меняется.

## Открытые вопросы

- **Гранулярность σ:** per-asset (перв. срез) vs per-(venue,asset). Агент привязан к площадке →
  per-venue точнее, но σ по активу проще и стабильнее. Начать per-asset, venue — расширение.
- **τ:** переиспользовать `CE_IMPACT_TAU_SEC` (recovery) или отдельный горизонт хеджа. Пока reuse.
- **Калибровка σ_ref:** зафиксировать по текущей наблюдаемой σ BTC на dev (owner-review при 705).
