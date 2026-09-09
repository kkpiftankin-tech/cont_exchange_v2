# F-18 — Капитал CE, чистая позиция и position-based хедж

> **Статус:** draft (docs-first). Код — после полной doc-цепочки (ADR-054 → feature → UC → sequences → contracts → data → tasks).

## Зачем

Сейчас у биржи CE **нет собственного капитала и позиции**. F-05A money-path
(ADR-049) хеджирует **каждый поток сегмента отдельно** (per-segment gross):
поток `x_i > 0` → отдельный `ExecutionIntent`. Это не нетит встречные ноги,
не отражает чистую позицию биржи и не связано с её капиталом.

F-18 вводит:

1. **Капитал CE** `K = {asset → amount}` — мультивалютный, seed из конфига,
   пополняется realized PnL/fees.
2. **Чистую позицию CE** `pos = {asset → net_qty}` — накопленную по каждому
   активу.
3. **Δпозиции из клиринга** — проекцию вектора клиринга `x` на активы после
   каждого converged батча.
4. **Хедж от net-позиции** — `|pos_a| > θ_a` → один хедж на актив (flatten),
   вместо per-segment gross.

## Конвейер

```text
order books ──(F-11/F-05A)──▶ liquidity curves ──(matching)──▶ clearing x, w=log-prices
                                                                     │
                                                          ce_position_projector
                                                            Δpos_a = Σ x_i·c_a
                                                                     │
                                                     pos_a += Δpos_a  (PG ce_position)
                                                                     │
                                                     |pos_a| > θ_a ?  ── нет ──▶ (ничего)
                                                                     │ да
                                                        ce_net_hedge_builder
                                                     ExecutionIntent(flatten a)
                                                                     │
                                              external-venues ▶ ExecutionReport
                                                                     │
                                          ledger: realized PnL/fees → ce_capital
                                                     pos_a -= filled_qty
```

## Проекция клиринга на активы

Для двустороннего сегмента `(venue, BASE/QUOTE)` с потоком `x_i` (знаковый:
`x_i>0` — покупка base, `x_i<0` — продажа) и эффективной ценой `P_eff` (из
`w[quote]`, ADR-052/053):

$$
\Delta pos_{\text{BASE}} \mathrel{+}= x_i,\qquad
\Delta pos_{\text{QUOTE}} \mathrel{-}= x_i\cdot P_{\text{eff}}.
$$

Встречные ноги неттятся автоматически: `+0.5 BTC` (venue A) и `−0.5 BTC`
(venue B) → `Δpos_BTC = 0` → хедж не нужен.

## Правило хеджа (flatten)

$$
|pos_a| > \theta_a \Rightarrow
\text{hedge}(a,\ \text{qty}=|pos_a|,\ \text{side}=
\begin{cases} \text{SELL}, & pos_a>0\\ \text{BUY}, & pos_a<0 \end{cases}).
$$

## Учётное тождество (канон, ADR-054 §6)

Единая книга — вектор остатков `h_a` (`ce_capital.balance`). Всё выводится:

| Параметр | Формула |
|---|---|
| Позиция (хеджируется) | `pos_a = h_a − target_a` |
| Mark (в numeraire) | `mark_a = exp(w[a] − w[N])`; `mark_N = 1` |
| Equity | `E = Σ_a h_a · mark_a` |
| Unrealized PnL | `uPnL_a = pos_a·(mark_a − cost_basis_a)` |
| Realized PnL | `rPnL += (P_fill − cost_basis)·q_closed·sgn(pos) − fee` (F-12) |
| **Тождество** | `E ≈ seed_equity + Σ rPnL + Σ uPnL − Σ fee` (`identity_ok`) |

Numeraire `N` (default USDT) — расчётная валюта: `mark=1`, `θ=∞`, **не хеджируется**.

## Гейтинг

| env | смысл | default |
|---|---|---|
| `F05A_MONEY_ENABLED` | money-path вкл (существует) | 0 |
| `CE_NET_HEDGE_ENABLED` | net-хедж вместо per-segment | 0 |
| `CE_NUMERAIRE` | расчётная валюта (mark=1, не хеджируется) | USDT |
| `CE_CAPITAL_SEED_<ASSET>` | seed капитала (напр. `CE_CAPITAL_SEED_BTC=10`) | 0 |
| `CE_HEDGE_THRESHOLD_<ASSET>` | порог θ_a | ∞ (нет хеджа) |
| `CE_HEDGE_MODE` | `FLATTEN` (в 0) или `TO_BAND` (до края θ) | FLATTEN |
| `CE_HEDGE_QUOTE_REF_<ASSET>` | reference-quote для хедж-инструмента | USDT |
| `CE_HEDGE_EQUITY_FRACTION` | κ: θ как доля equity (опц.) | 0 (off) |
| `CE_MAX_POSITION_<ASSET>` | жёсткий inventory cap (форс-хедж) | ∞ |

При `CE_NET_HEDGE_ENABLED=0` поведение биржи идентично до-F18 (legacy
per-segment путь ADR-049 сохранён).

## Трассировка

| Артефакт | Путь |
|---|---|
| ADR | [ADR-054](../../../03-architecture/adr/ADR-054-ce-capital-net-position-hedging.md) |
| Use Cases | [UC-F18-01](../../use-cases/UC-F18-01-form-ce-capital/), [UC-F18-02](../../use-cases/UC-F18-02-derive-ce-position/), [UC-F18-03](../../use-cases/UC-F18-03-hedge-net-position/) |
| L0 System seq | UC-F18-*/sequences/SEQ-UC-F18-*-system.md |
| L1 Service seq | [SEQ-F18-UC-F18-02-services](../../../05-components/sequences/SEQ-F18-UC-F18-02-services.md) |
| Contracts | `contracts/proto/fob/treasury/v1/treasury.proto` |
| Data | [ce-capital](../../../07-data/ce-capital.md), [ce-position](../../../07-data/ce-position.md) |
| Tasks | [F-18 tasks](../../../implementation-plan/F-18-ce-capital-position-hedge.tasks.md) |
