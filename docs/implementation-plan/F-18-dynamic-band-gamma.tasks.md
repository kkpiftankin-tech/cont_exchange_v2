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
**Приоритет:** Средний. **Файлы:** BFF `frontend/api/server.js`, drill-down.
- В agent-detail показать σ актива, γ, τ, итоговые `z_lim/z_mkt` и режим (D1/D2) — реальным DTO-путём
  (память `frontend-observability-real-dto-path`), без вычислений в браузере.
- **Приёмка:** на вкладке видно, откуда порог (плоский vs `γσ²τ`) и текущая σ.

### T-F18-705 — Проверка масштабирования + калибровка
**Приоритет:** Высокий. **Файлы:** `Testing/` probe или расширение существующего.
- На dev с `CE_BAND_GAMMA_MODE=1`: пороги активов различаются по волатильности; при σ_ref совпадают с
  D2; позиции сходятся к band; lag 0 (throughput не задет).
- **Приёмка:** высоковолатильный актив имеет больший порог, чем спокойный; калибровка при σ_ref держит прежний ~18.

## Порядок

701 (схема) → 702 (σ writer) → 703 (ledger reader+Γ, флаг off по умолчанию) → 704 (обзор) →
705 (включить флаг на dev, калибровать, проверить). Флаг off до 705 — прод-поведение не меняется.

## Открытые вопросы

- **Гранулярность σ:** per-asset (перв. срез) vs per-(venue,asset). Агент привязан к площадке →
  per-venue точнее, но σ по активу проще и стабильнее. Начать per-asset, venue — расширение.
- **τ:** переиспользовать `CE_IMPACT_TAU_SEC` (recovery) или отдельный горизонт хеджа. Пока reuse.
- **Калибровка σ_ref:** зафиксировать по текущей наблюдаемой σ BTC на dev (owner-review при 705).
