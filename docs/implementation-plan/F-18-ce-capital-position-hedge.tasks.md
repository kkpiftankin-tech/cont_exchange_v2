# Implementation Tasks: F-18 — CE Capital, Net Position & Position-Based Hedge

## Source Artifacts

- ADR: [ADR-054](../03-architecture/adr/ADR-054-ce-capital-net-position-hedging.md) — capital → net position → position-based hedge (**proposed**, решения владельца 2026-09-08)
- Feature: [F-18](../02-system/features/F-18-ce-capital-position-hedge/) (`feature.yaml` + README)
- Use cases: [UC-F18-01](../02-system/use-cases/UC-F18-01-form-ce-capital/), [UC-F18-02](../02-system/use-cases/UC-F18-02-derive-ce-position/), [UC-F18-03](../02-system/use-cases/UC-F18-03-hedge-net-position/)
- Sequences: L0 `SEQ-UC-F18-0{1,2,3}-system.md`, L1 [SEQ-F18-UC-F18-02-services](../05-components/sequences/SEQ-F18-UC-F18-02-services.md)
- Contracts: `contracts/proto/fob/treasury/v1/treasury.proto`
- Data: [ce-capital](../07-data/ce-capital.md), [ce-position](../07-data/ce-position.md)
- Reused: [ADR-049](../03-architecture/adr/ADR-049-*.md) money-path, [ADR-052/053](../03-architecture/adr/) log-price + safe-translator (P_eff), F-12 `PreHedgeCheck` / `execution.intents`

## Preconditions (docs-as-code gate)

- [x] ADR-054 (proposed)
- [x] Feature docs + YAML (F-18)
- [x] Use cases UC-F18-01..03 + L0 system sequences
- [x] L1 service sequence + contract/data binding tables
- [x] Contract `treasury.proto` (компилируется через contracts glob)
- [x] Data schemas: PG `ce_capital`/`ce_position`, CH `ce_position_history`
- [x] **ADR-054 → `accepted`** (решение владельца, 2026-09-08) — код Phase 1+ разблокирован
- [x] Business rules §F-18 (R-F18-001/002/003) в `docs/04-domain/business-rules.md`
- [ ] Test plan `docs/10-testing/features/F-18-test-plan.md`

## Размещение (ADR-054 §10, ИТОГ 2026-09-09) — переопределяет план ниже

**Позиция = Net Open Position (NOP)** по валюте (универсальная формула, вырождается
в собственный инвентарь при нуле клиентов). Разделение по фактическим ролям сервисов:

- **ledger** — владелец **settlement-балансов**: клиентские `accounts` (обязательства)
  + `venue_balances_` (собственные средства на венью; сейчас in-memory → персистить/
  replay) + house-аккаунт капитала (seed). Балансы двигают fills/исполнения, которые
  ledger **уже потребляет** (`batch.outputs`, `execution.venue`). Выдаёт балансы/NOP по gRPC.
- **risk** — считает **NOP + лимиты θ + размер хеджа** (риск-метрика; risk уже читает позиции).
- **execution router** — `matching execution_planner` (venue+split) + `venues` (child orders) — **есть (F-12)**.
- **frontend** — читает NOP у `risk`, балансы у `ledger` (UI → сервис → хранилище).

> **⚠️ Планы Phase 2–5 НИЖЕ (matching-центричные: проекция x, `ce_*` в matching,
> `TreasuryService` в matching) — ОТМЕНЕНЫ §10.** Заменены новым планом Phase R/L/K/E/F.
> Уже написанный matching-код `ce_*` подлежит удалению (T-F18-R00).

## Architecture decisions (обновлено §10)

| # | Вопрос | Решение |
| --- | --- | --- |
| D1 | Капитал | seed + realized PnL/fees; house-аккаунт в ledger |
| D2 | Определение позиции | **NOP** = собств. активы − клиентские обязательства (универсально; numeraire исключён) |
| D3 | Владелец балансов | **ledger** (не matching) — реюз `accounts`/`venue_balances_`/`balances_` |
| D4 | Расчёт NOP + размер хеджа | **risk** (не matching) |
| D5 | Исполнение | execution router = matching `execution_planner` + venues (есть) |
| D6 | Гейтинг | `CE_NET_HEDGE_ENABLED` (default off); legacy per-segment сохранён |

## Critical path (ADR-054 §10)

`R (снять matching ce_*)` → `L (ledger: house-аккаунт + seed + persist venue_balances + read RPC)` →
`K (risk: расчёт NOP + размер хеджа)` → `E (router wiring: risk→execution.intents→venues, есть)` →
`F (frontend: NOP у risk, балансы у ledger)`.

## Tasks (ADR-054 §10) — АВТОРИТЕТНО (заменяет Phase 0–5 ниже)

### Phase R — снять ошибочную реализацию в matching
- [x] **T-F18-R00.** Удалить `ce_position_projector.*`, `postgres_ce_treasury_repository.*`,
  `grpc_treasury_service.*`; реверт правок `matching_loop.{hpp,cpp}`/`main.cpp`/`CMakeLists.txt`;
  убрать `ce_*` из `init.sql`. Done 2026-09-09 (matching чист).

### Phase L — ledger: house-аккаунт + балансы + read-API
- **T-F18-L01.** Контракт: `ledger.proto` `GetExchangeBalances` (+ `ExchangeCurrencyBalance`) — компилируется. Doc `docs/06-api/grpc/ledger-exchange-balances.md`.
- **T-F18-L02.** Seed house-аккаунта: ledger при старте читает `CE_CAPITAL_SEED_<CCY>` → заводит `accounts['__ce_house__']` (idempotent). `cpp/ledger/src/main.cpp` + `ledger_uc`.
- **T-F18-L03.** `GetExchangeBalances`: собрать валютный вектор = house-аккаунт + Σ `venue_balances_` + Σ клиентских обязательств (по валюте). `ledger_uc.cpp` + `grpc_ledger_service.cpp`.
- **T-F18-L04.** Устойчивость `venue_balances_`: rebuild из Kafka-replay ИЛИ таблица `venue_balances` (решение здесь). AC F18-3.

### Phase K — risk: NOP + размер хеджа
- **T-F18-K01.** `risk_uc`: `ComputeNOP` = активы−обязательства per-currency (читает `LedgerService.GetExchangeBalances`); numeraire исключён. Unit-тест (A⊇B). AC F18-4,5.
- **T-F18-K02.** Размер хеджа: `|NOP|>θ` → qty (FLATTEN/TO_BAND), side; env `CE_HEDGE_THRESHOLD_<CCY>`/`CE_HEDGE_MODE` (в risk). Эмиссия hedge-intent в `execution.intents` за `CE_NET_HEDGE_ENABLED`. AC F18-6,9.
- **T-F18-K03.** `GetRiskSnapshot` расширить NOP/exposure по валюте (для UI). AC F18-8.

### Phase E — execution router (в основном есть, F-12)
- **T-F18-E01.** Убедиться, что hedge-intent от risk проходит `PreHedgeCheck` → `execution_planner` (venue split) → venues → `execution.venue` → ledger уменьшает NOP. AC F18-7.

### Phase F — frontend
- **T-F18-F01.** frontend-api: `/api/v1/ce/treasury` агрегирует `LedgerService.GetExchangeBalances` (балансы) + risk NOP/exposure — через gRPC (не PG). Переиспользовать существующий gRPC-клиент.
- **T-F18-F02.** React `/ce-treasury-live`: рендер валютного вектора (ledger) + NOP/θ/хедж (risk). Чистый рендер.

---

## (УСТАРЕЛО — заменено Tasks §10 выше) Прежний план Phase 0–5

---

## Tasks

### Phase 0 — Docs & ADR (no code)

#### T-F18-000. ADR-054 → accepted
- Владелец подтверждает ADR-054 (`status: proposed → accepted`). Гейт на Phase 2+. Owner: solution-architect.

#### T-F18-00T. Test plan + business rules
- `docs/10-testing/features/F-18-test-plan.md` (U/IT/E2E из feature.yaml.tests). Business rule **R-F18-001** (проекция x→Δpos, flatten-хедж) → `docs/04-domain/business-rules.md`. Owner: test-architect + trading-domain-specialist.

### Phase 1 — Schema & contract

#### T-F18-001. PG/CH schema
- `infra/postgres/init.sql`: `ce_capital`, `ce_capital_applied`, `ce_position`. ClickHouse: `ce_position_history` (в `market_data` EnsureSchema или init). Из [ce-capital.md](../07-data/ce-capital.md) / [ce-position.md](../07-data/ce-position.md). Owner: data-schema-designer.

#### T-F18-002. Proto build + mappers
- Подтвердить компиляцию `treasury.proto` (glob уже подхватывает). Transport-mapper `CePosition/CeCapital` ↔ proto. Owner: proto-contract-designer + code-implementer.

### Phase 2 — Position projection (UC-F18-02)

#### T-F18-201. ce_position_projector (pure domain)
- `cpp/matching/src/app/ce_position_projector.{hpp,cpp}`: вход — clearing outcome (segments, x_i, w); выход — `map<asset, Decimal> Δh`. `ΔhBASE += x_i`, `ΔhQUOTE += -x_i·P_clr`, `P_clr = exp(w[quote]-w[base])`. Неттинг встречных ног. Только Decimal (§9). Unit: `ce_position_projector_test.cpp` (проекция, неттинг, знак). Owner: code-implementer. AC: F18-2, F18-3.

#### T-F18-202. ce_position persistence + idempotency
- `postgres_ce_treasury_repository`: UPSERT `ce_capital.balance` (Δh) и вью `ce_position` (pos=balance−target) с `last_batch_id` guard (idempotent), INSERT `ce_position_history`. Wiring в matching после converged `VectorClearingUseCase` (degraded → skip, Alt A1). Owner: code-implementer. AC: F18-4, F18-9.

#### T-F18-203. Cost-basis (WAC) + marks + MTM/equity (канон)
- В проекторе/репозитории: обновление `cost_basis` по WAC от `P_clr` при наборе позиции; marks `mark_a = exp(w[a]−w[N])` из клиринговых log-цен; numeraire `mark=1`. Расчёт `unrealized_pnl = pos·(mark−cost_basis)`, `equity = Σ balance·mark`, `identity_ok`. Unit: `ce_mtm_equity_test.cpp` (тождество Equity=seed_equity+rPnL+uPnL−fee; numeraire mark=1). Owner: code-implementer + trading-domain-specialist. AC: F18-10, F18-11, F18-12.

### Phase 3 — Net hedge (UC-F18-03)

#### T-F18-301. ce_net_hedge_builder
- `cpp/matching/src/app/ce_net_hedge_builder.{hpp,cpp}`: для каждого актива `|pos_a| > θ_a` (env `CE_HEDGE_THRESHOLD_<ASSET>`) → `CeNetHedgeIntent` (flatten side, qty=|pos_a|, instrument a/QUOTE_ref). Транслирует в `ExecutionIntent`. Гейт `CE_NET_HEDGE_ENABLED`. Unit: `ce_net_hedge_builder_test.cpp` (порог, side, qty, disabled). Owner: code-implementer. AC: F18-5, F18-6.

#### T-F18-302. PreHedgeCheck + publish + legacy switch
- Вызов `RiskService.PreHedgeCheck` (переиспользование F-12); ACCEPT → publish в `execution.intents` (key `ce|hedge|<asset>`). При `CE_NET_HEDGE_ENABLED=0` — `vector_clearing_hedge_builder` per-segment (без изменений). Owner: code-implementer. AC: F18-6, F18-9.

### Phase 4 — Capital PnL feedback (UC-F18-01)

#### T-F18-401. Seed capital
- CE Treasury при старте: seed `ce_capital` из `CE_CAPITAL_SEED_<ASSET>` (idempotent — только пустую строку). Owner: code-implementer. AC: F18-1.

#### T-F18-402. Realized PnL → capital + reduce pos
- `ledger`: на `ExecutionReport` net-хеджа применить realized PnL/fees → `ce_capital` (idempotent через `ce_capital_applied` по `report_id`), уменьшить `ce_position.net_qty` на `filled_qty`. Owner: code-implementer. AC: F18-7.

### Phase 5 — UI & tests

#### T-F18-501. TreasuryService read API + UI-панель наблюдаемости
- gRPC `GetCeTreasurySnapshot` (capital+position+equity+identity_ok); frontend-api `GET /api/v1/ce/treasury`; React `/ce-capital-live` — панель ADR-054 §8: per-asset таблица (balance/target/pos/mark/cost_basis/uPnL/rPnL/θ/hedge_armed/last Δpos) + агрегат (Equity=Σh·mark, Σ uPnL, Σ rPnL, seed_equity) + индикатор `identity_ok` ✔/❗ + подсветка hedge_armed. Frontend — только fetch+render (memory `frontend-no-domain-compute`). Owner: code-implementer + frontend-architect. AC: F18-8, F18-11.

#### T-F18-502. E2E
- `Testing/f18_ce_capital_e2e.sh`: seed → flow orders → clearing → Δpos в PG → net-hedge intent → ExecutionReport → `ce_capital` PnL update + `ce_position` reduce. Assert неттинг (встречные ноги → нет лишнего хеджа). Owner: test-architect. AC: F18-2,3,5,7.

---

## Acceptance coverage

| AC | Tasks |
| --- | --- |
| F18-1 seed capital | T-F18-401 |
| F18-2 projection | T-F18-201 |
| F18-3 netting | T-F18-201, T-F18-502 |
| F18-4 idempotent pos | T-F18-202 |
| F18-5 threshold hedge | T-F18-301 |
| F18-6 net replaces per-segment | T-F18-301, T-F18-302 |
| F18-7 PnL → capital (cost-basis) | T-F18-402 |
| F18-8 read API/UI snapshot | T-F18-501 |
| F18-9 no regression (flag off) | T-F18-202, T-F18-302 |
| F18-10 marks + numeraire | T-F18-203, T-F18-301 |
| F18-11 equity identity + panel | T-F18-203, T-F18-501 |
| F18-12 unrealized PnL + cost-basis | T-F18-203 |
