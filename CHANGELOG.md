# Changelog

Формат основан на [Keep a Changelog](https://keepachangelog.com/ru/1.0.0/).
Записи — по фичам (`F-XX`) и крупным изменениям. Подробности — в git-истории и PR.

## [Unreleased]

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
