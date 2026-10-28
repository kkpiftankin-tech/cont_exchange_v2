# Changelog

Формат основан на [Keep a Changelog](https://keepachangelog.com/ru/1.0.0/).
Записи — по фичам (`F-XX`) и крупным изменениям. Подробности — в git-истории и PR.

## [Unreleased]

### F-18 — Заработок агентов по биржам (ADR-087, CR-F18-002)

- Вкладка «PnL» (`/hedge-pnl-live`): таблица «Заработок агентов» — по каждому агенту R, U и R+U внутренней биржи CE, внешних бирж и итого (USDT), строка «все агенты».
- ledger: новый `GetAgentPnl`; книга сделок агента на внутренней бирже по средней себестоимости (`agent_pnl_book.hpp`), внешние суммы хедж-исполнений; таблица `ce_agent_pnl_book`, колонки `ext_*`, `rvf_base` в `ce_agent_position` (миграция — защитный блок, применять до выкладки); выключатель `CE_AGENT_PNL_ENABLED`.
- BFF: `GET /api/vector-clearing/agent-pnl` (пересылка без арифметики). Фронт только рисует.
- Внешний U считается на несопоставленном объёме (OQ-F18v2-26…29).
- Панель «Живые позиции», ряд для графика и вкладка «PnL» берут R, U, R+U и итоги из ledger (`GetAgentPnl`, итоги по видам `by_kind`); из BFF удалены формулы корзины сделок и круговой сделки, из панели — суммирование в браузере (ADR-087 п. 10, CR-F18-003). В истории `ce_agent_pnl_history` виден разрыв: до 2026-10-28 U писался по прежней формуле.

### F-09 — Batch / Combo / Multi-leg Orders (PR #13)

**MVP-1 (`orchestration_only`):**

- Контракты `combo.proto` / `execution_group.proto` + 5 rpc `OrderFlowService`
  (Create/CreateBatch/Preview/Cancel/Get combo); Kafka-топики `execution.groups`
  (+ `backtest.execution.groups`); 7 PG + 5 ClickHouse таблиц.
- order_flow: domain `ComboOrder` + инварианты (ADR-031/032/033), Postgres
  combo-репозиторий (атомарный insert 5 таблиц, идемпотентность), grouped
  producer (`orders.normalized`, backward-compatible), Create/Cancel use cases,
  gRPC-хендлеры. Проверено e2e (live) + 5 unit-сьютов.

**MVP-2 — grouped vector solver:**

- `cex::common::Decimal::div` (fixed-point деление, half-up) + unit-тест.
- matching domain: feasible caps, `GroupedSolverBisection`
  (strict/scalable/best_effort), child graph transitions (OCO/bracket,
  идемпотентные); app: `SolveGroupedBatch`.
- matching infra: `ExecutionGroups` producer (Kafka) + Postgres repo
  (идемпотентно, проверено против live PG) + active groups loader (live PG).
- Интеграция в `matching_loop` (gated на `MATCHING_POSTGRES_DSN`, аддитивно к
  single-leg F-04).
- **filled_cum convergence (GAP-1 закрыт):** partial-группы накапливают
  `filled_cum` идемпотентно; combo → filled при исчерпании binding-ноги
  (ratio-максимум); пустые batch'и не публикуются.
- **Feature flags + policy (002):** `ComboPolicy` (флаги `f09_*_enabled` +
  лимиты) — честный create-gate в order_flow (env `F09_*`).
- **ExecutionGroup enrichment (062):** proto += `user_id` + LegResult
  `instrument_symbol`/`side`; `combo_orders` += `user_id`/`account_id`.
- **Ledger grouped postings (060):** `ApplyExecutionGroup` over `execution.groups`
  consumer (идемпотентно по `execution_group_id`).
- **Полный E2E (090) проверен LIVE:** multileg combo → matching → `execution.groups`
  → ledger position владельца.
- **Risk grouped (040):** `GroupPreTradeCheck` domain (AC-F09-006 + notional/legs/
  external лимиты) + `PreTradeCheckGroup` gRPC + order_flow `RiskClient` wiring
  (заменил stub-approve, fail-closed). Verified live (accept + fail-closed proof).

**MVP-2 завершён.** Полная вертикаль: order_flow (policy+risk) → matching (solver)
→ execution.groups → ledger positions. Дальше — MVP-3+ (spread/factor QP,
OCO/bracket, external legs, frontend/observability).
**Известные нюансы (feature README):** `exec_price=0` для не котируемых символов;
Kafka `risk.alerts` publish для grouped reject — небольшой follow-up (сейчас лог).
