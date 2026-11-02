---
id: DOC-DATA-CE-ASSET-LIQUIDITY
phase: 07-data
status: planned (T-F18-L01 schema; L02 writer / L03 reader — в работе); flags CE_LIQ_SIGNAL_ENABLED / CE_BAND_LIQ_MODE off
level: sea
owner: core-team
source:
  - incoming-docs/2026-09-28-dealer-two-continuous-exchanges-48590e82.md (§5/§8 Q_i=ω_i c_i/A, A=σ√(2γ/Λ) ⇒ Q∝√Λ)
related:
  - docs/03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md (§D3 дизайн ликвидностной оси)
  - docs/implementation-plan/F-18-liquidity-band.tasks.md (T-F18-L01..L06)
  - docs/07-data/ce-asset-volatility.md (σ-ось, тот же паттерн writer→PG→reader)
  - docs/04-domain/calculations/CALC-CE-ANCHOR.md (эталон Q_i, A)
---

# Data: CE Asset Liquidity (depth для ликвидностной оси band)

> **Status:** 🔨 в работе (T-F18-L01 схема готова; L02 writer / L03 reader — далее).
> До включения флагов таблица не влияет на поведение (fallback `LiqScale=1.0`).

## Назначение

Глубина стакана `depth` (α_e — capital-depth CE-агента, тыс.USDT/‰) для **ликвидностной оси**
динамического порога band поверх O1 (`Γ=γσ²τ`) и волатильностной оси D1. По
[ADR-066 §D3](../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md):
`LiqScale = clamp((depth/depth_ref)^0.5, lr_min, lr_max)` — глубже рынок ⇒ ШИРЕ band (можно
держать больший запас). Степень `p=0.5` — по IN-017 `Q_i ∝ √Λ` (частичная, не полная, реализация O2).

## Owner

- **Writer: matching** (α_e — конфиг matching `CE_TRANSFER_ALPHA_*`/`CE_STOCK_ALPHA_*`, НЕ
  рыночный сигнал market_data). throttled-UPSERT per `(asset, venue)` за флагом `CE_LIQ_SIGNAL_ENABLED`.
- **Reader: ledger** (`LoadAssetDepth`, TTL-кэш как `LoadAssetSigma`) — считает `LiqScale`,
  композирует `total_scale = clamp(VolScaleFactor·LiqScale, tr_min, tr_max)` за флагом `CE_BAND_LIQ_MODE`.

Отдельная таблица (НЕ колонка в `ce_asset_volatility`): разные писатели (matching vs market_data)
на одну строку = гонка UPSERT.

## PostgreSQL (OLTP)

```sql
CREATE TABLE IF NOT EXISTS ce_asset_liquidity (
    asset       TEXT NOT NULL,                       -- инструмент-символ без слэша ('BTCUSDT'|'ETHBTC')
    venue       TEXT NOT NULL DEFAULT '',            -- площадка; '' = агрегат
    depth       NUMERIC(38, 18) NOT NULL DEFAULT 0,  -- α_e, тыс.USDT/‰
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (asset, venue)
);
CREATE INDEX IF NOT EXISTS ce_asset_liquidity_updated_idx ON ce_asset_liquidity (updated_at DESC);
```

Ключ `(asset, venue)` совпадает с гранулярностью band-breach `(agent_id, asset, venue)` — ledger
берёт depth ровно для площадки агента.

## Единицы и depth→порог

`depth` — тыс.USDT/‰ (α_e). Масштаб к порогу — на стороне ledger: `LiqScale=(depth/depth_ref)^0.5`,
`depth_ref`=`CE_BAND_DEPTH_REF`. Пример: `depth=2·depth_ref ⇒ LiqScale=√2≈1.41` (band шире на 41%).

## Staleness / fallback

Строки нет / `updated_at` старше порога ⇒ ledger `LiqScale=1.0` (нейтрально, band как до фичи).
depth — конфиг (не EWMA), поэтому min-samples guard не нужен (в отличие от σ).

## Conflict Notes

- Это **частичная** реализация O2 из IN-017 (`√`-зависимость согласована), но сигнал — внутренний
  прокси α_e, НЕ внешняя `Λ=λ1+λ2` из книг площадок. Полный O2 (реальная `Λ` из F-11
  `SideLiquidityCurve`) — owner-gate, см. ADR-066 §Addendum.
