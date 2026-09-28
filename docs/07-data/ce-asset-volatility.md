---
id: DOC-DATA-CE-ASSET-VOLATILITY
phase: 07-data
status: planned (T-F18-701 schema added; writer T-F18-702, reader T-F18-703)
level: sea
owner: core-team
source:
  - incoming-docs/2026-08-06-Кривые_котирования_внутренних_маркет-мейкеров_CE_биржевое_из-e70752d0.md (§6.1 ρ≈γσ²τ, §6.4 Z̄=clim/Γ)
related:
  - docs/03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md (Γ=ρ=γσ²τ, дизайн D1)
  - docs/implementation-plan/F-18-dynamic-band-gamma.tasks.md (T-F18-701..705)
  - docs/07-data/ce-agent-position.md (позиция c_j, порог которой масштабирует σ)
---

# Data: CE Asset Volatility (σ для динамического порога band)

> **Status:** 🟡 schema-only (T-F18-701, 2026-09-28). Таблица `ce_asset_volatility` добавлена в
> `infra/postgres/init.sql`. Writer (market_data EWMA-σ, T-F18-702) и reader (ledger `Γ=γσ²τ`,
> T-F18-703) — planned. До включения `CE_BAND_GAMMA_MODE=1` таблица не влияет на поведение
> (ledger использует плоский `ce_band_fee_k`, ADR-066 D2).

## Назначение

`ce_asset_volatility` — оценка **волатильности σ актива**, из которой ledger выводит
**динамический порог band** `Γ = γ·σ²·τ` и `z_lim = clim/Γ` (Кривые §6.4). По базовым докам
`Γ = ρ = γσ²τ` — тот же коэффициент риск-неприятия, что и снос якоря (skew); см.
[ADR-066](../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md). Смысл: чем
волатильнее актив, тем выше риск незахеджированного запаса → шире порог до внешнего хеджа.

## Owner

- **Writer: market_data** — считает EWMA реализованной волатильности лог-доходностей mid-цены
  per (venue, asset), пишет неблокирующе (паттерн `fill_diagnostics`: очередь + воркер +
  переиспользуемое соединение, T-F18-601/602).
- **Reader: ledger** — `detect_and_emit_band_breach_locked` читает σ с TTL-кэшем (паттерн
  `LoadBandFeeConfig` / `f05a_clearing_config`), считает `Γ=γσ²τ`. Также **BFF/observability**
  (T-F18-704) — показать σ и итоговые пороги.

## PostgreSQL (OLTP)

| Колонка | Тип | Смысл |
| --- | --- | --- |
| `asset` | TEXT | Актив (`BTC`/`ETH`/`SOL`), часть PK |
| `venue` | TEXT | Площадка; `''` = агрегат по площадкам (первый срез per-asset), часть PK |
| `sigma` | NUMERIC(38,18) | σ лог-доходности mid (**безразмерная доля** за окно), EWMA |
| `window_sec` | INT | Окно/полупериод EWMA (диагностика, не влияет на чтение) |
| `samples` | INT | Сколько тиков вошло — доверие к σ (мало → ledger fallback) |
| `updated_at` | TIMESTAMPTZ | Свежесть; старше порога → ledger fallback на `ce_band_fee_k` |

PK `(asset, venue)`. Индекс по `updated_at DESC` (staleness-мониторинг).

## Единицы и σ→порог

`sigma` — безразмерная доля (σ лог-доходности за окно оценки). Масштаб к горизонту τ и перевод в
порог — на стороне ledger: `Γ = γ·σ²·τ` (γ = `f05a_clearing_config.gamma`, τ = env `CE_BAND_TAU_SEC`,
дефолт = `CE_IMPACT_TAU_SEC`=5с), затем `z_lim[k-USDT] = clim/Γ` с калибровочной константой
`CE_BAND_GAMMA0` (при σ_ref порог ≈ текущий ~18@10bps). Детали — ADR-066 §«D1: дизайн».

## Staleness / fallback

`updated_at` старше порога **или** `samples` мало ⇒ ledger игнорирует σ и берёт плоский
`ce_band_fee_k` (обратная совместимость). Режим (dynamic/flat) логируется в `band breach emitted`.

## Conflict Notes

Гранулярность σ: первый срез — **per-asset** (`venue=''`). Агент привязан к площадке, поэтому
per-(venue,asset) точнее, но σ по активу проще/стабильнее. Переход на per-venue — расширение
(отдельная строка на площадку), без изменения схемы.
