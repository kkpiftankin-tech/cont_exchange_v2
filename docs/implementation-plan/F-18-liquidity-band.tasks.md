# F-18 #7 (scoped) — Ликвидностная ось band: implementation tasks

> Дизайн: [ADR-066 §D3](../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md).
> Scoped-вариант (НЕ O2): масштаб band по глубине стакана `depth`=α_e поверх O1 (`Γ=γσ²τ`) + D1 (вола).
> `total_scale = clamp(VolScaleFactor·LiqScale, tr_min, tr_max)`, `LiqScale=clamp((depth/depth_ref)^0.5, lr_min, lr_max)`.
> Money-path (ledger-порог) ⇒ `code-reviewer` обязателен. Всё за флагами, обратимо.

## Стадии

| ID | Задача | Файлы | Флаг | Статус |
|---|---|---|---|---|
| T-F18-L01 | PG-схема `ce_asset_liquidity(asset, venue, depth NUMERIC, updated_at)` + индекс `(asset,venue)` + create на dev | `infra/postgres/init.sql`; doc `docs/07-data/ce-asset-liquidity.md` | — | ✅ done |
| T-F18-L02 | matching: throttled-UPSERT `depth`=α_e per `(asset,venue)` в `ce_asset_liquidity` (`UpsertBatch` — 1 транзакция на батч, perf-фикс code-review) за `CE_LIQ_SIGNAL_ENABLED` | `cpp/matching/src/app/matching_loop.cpp` + repo `postgres_asset_liquidity_repository.{hpp,cpp}` | `CE_LIQ_SIGNAL_ENABLED` | ✅ done |
| T-F18-L03 | ledger: `LoadAssetDepth(asset,venue)` (копия `LoadAssetSigma`, TTL-кэш, `poll_conn_`, staleness→1.0) + static `LiqScaleFactor` + static `BandTotalScale` (композиция, двойной клэмп) в `detect_and_emit_band_breach_locked` | `cpp/ledger/src/app/ledger_uc.{hpp,cpp}` | `CE_BAND_LIQ_MODE` | ✅ done |
| T-F18-L04 | unit-тест: `LiqScaleFactor` (монотонность, clamp, `p=0.5` √2) + `BandTotalScale` (LIQ-2 двойной клэмп, LIQ-3 обе off→1.0, vol/liq-only) | `cpp/ledger/tests/ce_band_liq_scale_test.cpp` + CMake | — | ✅ done (17/17) |
| T-F18-L05 | docs: `CALC-CE-ANCHOR` §5/§7 (ось реализована scoped), business-rules §F-18, `ce-asset-liquidity.md`, `feature.yaml`, status #7 | `docs/**` | — | ✅ done |
| T-F18-L06 | dev-деплой + e2e: **✅ ПОДТВЕРЖДЕНО (2026-09-30)** — `ce_asset_liquidity` наполняется matching'ом (α per-venue: BTC/uniswap 0.64…ETH/okx 1140), ledger дифференцирует band точно по формуле: SOL/okx depth346→z_mkt 36 (×2 clamp), ETH/coinbase 0.82→11.497 (×0.639), cross-pair 0.0002→9 (×0.5 clamp), нет строки→×1.0 (LIQ-4). code-review money-path пройден (блокеры закрыты). Флаги на dev ON для верификации — постоянное включение owner-gate | — | — | ✅ done |

## Инварианты (проверить в тесте + review)

- `LIQ-1`: `LiqScale ∈ [lr_min, lr_max]`, монотонно не убывает по depth.
- `LIQ-2`: `total_scale ∈ [tr_min, tr_max]` всегда (финальный клэмп поверх Vol×Liq).
- `LIQ-3`: `CE_BAND_LIQ_MODE=0` ⇒ `LiqScale≡1.0` ⇒ поведение идентично до фичи (обратимость).
- `LIQ-4`: staleness/отсутствие depth ⇒ `LiqScale=1.0` (нейтрально, без падения band).
- Деньги — `Decimal` для порогов; `depth`/`scale` — `double` (research-диагностика, не в ledger-баланс).

## Калибровка (env, dev)

`CE_BAND_DEPTH_REF` (тыс.USDT/‰, якорь) · `CE_BAND_LIQ_RATIO_MIN/MAX=[0.5,2.0]` ·
`CE_BAND_TOTAL_RATIO_MIN/MAX=[0.1,15]`. Подобрать `depth_ref` по медиане живых α на dev
(как `CE_BAND_SIGMA_REF=1.5e-4` для σ). Caveat: если α почти константа per asset → ось почти
нейтральна (ожидаемо; ценность растёт при реальной per-venue дифференциации глубины).

## Открытое / связь

- Это частичная реализация O2 (`√`-зависимость согласована с `Q∝√Λ`), но сигнал — внутренний
  прокси α_e, не внешняя `Λ`. Полный O2 (реальная `Λ` из F-11 `SideLiquidityCurve`) — owner-gate.
- После L06 обновить `F-18-ce-band-hedge.status.md` #7 (ликвидностная ось done scoped).
- **Follow-up (perf, code-review non-blocking):** при обеих осях on `detect_and_emit_band_breach_locked`
  делает ДВА синхронных PG round-trip (`LoadAssetSigma`+`LoadAssetDepth`) на cache-miss под глобальным
  `mu_` — усиливает существующий D1-паттерн блокировки главного ledger-мьютекса на сетевой I/O.
  TTL-кэш ~1с смягчает; полный фикс (async-prefetch/вынести из-под `mu_`) — отдельная задача,
  общая с D1 (не только #7). Не блокирует scoped-фичу (за флагом).
