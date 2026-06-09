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
  single-leg F-04). **Live end-to-end:** gRPC create multileg combo →
  `execution_groups` (PG) + `execution.groups` (Kafka).

**Известные гэпы (см. feature README):** фидбэк `filled_cum` для partial-групп,
ledger grouped postings (060), risk `PreTradeCheckGroup` (040), полный E2E (090).
