---
id: DOC-DATA-CE-POSITION
phase: 07-data
status: schema-proposed-impl-pending
owner: core-team
source:
  - ADR-054 §10 «Определение позиции (NOP) и размещение»
related:
  - docs/07-data/ce-capital.md
  - docs/02-system/features/F-18-ce-capital-position-hedge/
  - contracts/proto/fob/risk/v1/risk.proto
---

# Позиция биржи CE = Net Open Position (NOP) — производная, считает `risk`

> **ADR-054 §10:** «позиция биржи» — это **NOP** (чистая экспозиция по валюте), а
> НЕ хранимый баланс и НЕ проекция клиринга. NOP **не имеет своей таблицы** —
> выводится из балансов ledger. Считает её **`risk`** (риск-метрика). Снятая
> интерим-таблица `ce_position`/`ce_position_history` в matching отменена.

## Определение (универсальное)

$$
\text{NOP}_a=\underbrace{\Big(\text{капитал}_a+\textstyle\sum_v \text{venue\_bal}_{a,v}\Big)}_{\text{активы биржи в }a}-\underbrace{\textstyle\sum_u \text{client\_bal}_{a,u}}_{\text{обязательства}},\qquad a\neq N.
$$

- **Универсально (A ⊇ B):** нет клиентов ⇒ `Σ client_bal=0` ⇒ NOP = собственный
  инвентарь; появляются клиенты ⇒ член обязательств активируется. Одна формула.
- `N` (numeraire, `CE_NUMERAIRE`, default USDT) исключён: `θ_N=∞`, не хеджируется.
- Сегрегированная клиентская кастодия само-гасится (актив ↔ обязательство) → в
  риск не входит; хеджируется только собственная экспозиция биржи.

## Хедж по NOP

$$
|\text{NOP}_a| > \theta_a \Rightarrow \text{hedge}(\text{qty},\ \text{side}):\
\text{излишек}(NOP>0)\to \text{SELL},\ \text{дефицит}(NOP<0)\to \text{BUY}.
$$

`FLATTEN`: `qty=|NOP_a|`; `TO_BAND`: `qty=|NOP_a|−θ_a` (`CE_HEDGE_MODE`).
Пороги `θ_a` — env `CE_HEDGE_THRESHOLD_<CCY>` (в `risk`, не в matching); опц.
масштаб от equity (`CE_HEDGE_EQUITY_FRACTION`).

## Источники данных (что читает `risk`)

| Компонент NOP | Откуда | Владелец |
|---|---|---|
| `капитал_a`, `venue_bal_a` (активы биржи) | ledger house-аккаунт + `venue_balances_` | ledger |
| `client_bal_a` (обязательства) | ledger `accounts` (агрегат по клиентам) | ledger |
| `mark_a` (оценка в numeraire) | клиринговые log-цены (ADR-052) / market_data | market_data |

`risk` читает балансы у ledger (RPC), считает NOP, применяет θ/лимиты, определяет
размер хеджа → эмиттит хедж-намерение в execution router.

## Хранение

- **NOP — не хранится** (производная). Может кэшироваться/логироваться в
  `risk_snapshots` для наблюдаемости (per-currency exposure), но истина — балансы.
- История ДО→Δ→ПОСЛЕ по клирингу (для UI) — опционально из risk-снапшотов/логов;
  таблицы `ce_position_history` больше нет.

## Связи

- Балансы: [ce-capital.md](ce-capital.md) (ledger).
- Контракт: `fob.risk.v1.RiskService` (NOP + hedge decision), `LedgerService.GetExchangeBalances`.
