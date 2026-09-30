# F-18 #7 (scoped) — Ликвидностная ось band: implementation tasks

> Дизайн: [ADR-066 §D3](../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md).
> Scoped-вариант (НЕ O2): масштаб band по глубине стакана `depth`=α_e поверх O1 (`Γ=γσ²τ`) + D1 (вола).
> `total_scale = clamp(VolScaleFactor·LiqScale, tr_min, tr_max)`, `LiqScale=clamp((depth/depth_ref)^0.5, lr_min, lr_max)`.
> Money-path (ledger-порог) ⇒ `code-reviewer` обязателен. Всё за флагами, обратимо.

## Стадии

| ID | Задача | Файлы | Флаг | Статус |
|---|---|---|---|---|
| T-F18-L01 | PG-схема `ce_asset_liquidity(asset, venue, depth NUMERIC, updated_at)` + индекс `(asset,venue)` + ALTER на dev | `infra/postgres/init.sql`; doc `docs/07-data/ce-asset-liquidity.md` | — | planned |
| T-F18-L02 | matching: throttled-UPSERT `depth`=α_e per `(asset,venue)` в `ce_asset_liquidity` (паттерн EWMA-σ writer T-F18-702, но α — конфиг, без EWMA) | `cpp/matching/src/app/matching_loop.cpp` + новый repo `cpp/matching/src/infra/postgres/postgres_asset_liquidity_repository.{hpp,cpp}` | `CE_LIQ_SIGNAL_ENABLED` | planned |
| T-F18-L03 | ledger: `LoadAssetDepth(asset,venue)` (копия `LoadAssetSigma`, TTL-кэш, `poll_conn_`, staleness→1.0) + static `LiqScaleFactor(depth,depth_ref,p,lr_min,lr_max)` + композиция `total_scale` в `detect_and_emit_band_breach_locked` (двойной клэмп) | `cpp/ledger/src/app/ledger_uc.{hpp,cpp}` | `CE_BAND_LIQ_MODE` | planned |
| T-F18-L04 | unit-тест: `LiqScaleFactor` (монотонность, clamp `[lr_min,lr_max]`, `p=0.5` √2-пример) + `total_scale` двойной клэмп (Vol×Liq в `[tr_min,tr_max]`) | `cpp/ledger/tests/ce_band_liq_scale_test.cpp` + CMake | — | planned |
| T-F18-L05 | docs: карточка `CALC-CE-ANCHOR` §5/§7 (ликвидностная ось — частичный O2), business-rules §F-18 (упоминание оси), `docs/07-data/ce-asset-liquidity.md` | `docs/04-domain/**`, `docs/07-data/**` | — | planned |
| T-F18-L06 | dev-деплой (matching + ledger, ~30 мин) + force-recreate + e2e: `ce_asset_liquidity` наполняется, per-venue дифференциация band (глубокий venue ⇒ шире); `code-reviewer` money-path | — | — | planned |

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
