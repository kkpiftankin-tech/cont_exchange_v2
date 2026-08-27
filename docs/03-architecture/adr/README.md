# Architecture Decision Records

Журнал архитектурных решений. Новый ADR создаётся при любом значимом архитектурном выборе.

## Нумерация и статусы

- Единый формат `ADR-NNN` (трёхзначный, без вариантов `ADR-000N`).
- `status` синхронизирован между этим реестром, frontmatter и телом ADR.
- Допустимые статусы: `accepted`, `proposed`, `planned`, `deprecated`, `superseded`.
- Все ADR используют YAML-frontmatter (`id` / `status` / `date` / `owners` / `related`).

## Реестр

| ID | Title | Status |
| --- | --- | --- |
| ADR-001 | Event-driven C++ microservices | [accepted](ADR-001-event-driven-microservices.md) |
| ADR-002 | Protobuf / gRPC / Kafka payload contracts as source of truth | [accepted](ADR-002-protobuf-grpc-contracts.md) |
| ADR-003 | Kafka-совместимый брокер (Redpanda / Kafka) | [accepted](ADR-003-kafka-redpanda-broker.md) |
| ADR-004 | PostgreSQL OLTP + ClickHouse OLAP | [accepted](ADR-004-postgres-clickhouse-data-platform.md) |
| ADR-005 | Fixed-point Decimal for money, qty, price, fee, PnL | [accepted](ADR-005-fixed-point-decimal-money.md) |
| ADR-006 | Docs-as-code repository structure | [accepted](ADR-006-docs-as-code-repository-structure.md) |
| ADR-007 | Feature traceability gate | [accepted](ADR-007-feature-traceability-gate.md) |
| ADR-008 | Code/Doc drift policy | [accepted](ADR-008-code-doc-drift-policy.md) |
| ADR-009 | Shadow-mode isolation strategy for F-15 Replay | [accepted](ADR-009-shadow-mode-isolation-strategy.md) |
| ADR-010 | AgentLog vs ReplayAgentLog unification strategy | [proposed](ADR-010-agentlog-replay-agentlog-unification.md) |
| ADR-011 | replay_agentlogs DDL placement | [accepted](ADR-011-replay-agentlogs-ddl-placement.md) |
| ADR-012 | Venue Execution Adapter logical decomposition | [accepted](ADR-012-venue-execution-adapter-decomposition.md) |
| ADR-013 | Execution Planning placement (library vs service) | [accepted](ADR-013-execution-planning-placement.md) |
| ADR-014 | `cpp/venues` runtime packaging for F-11/F-12 | [accepted](ADR-014-venues-binary-vs-components.md) |
| ADR-015 | F-20 sim.execution.venue strict topic isolation | [accepted](ADR-015-sim-execution-topic-isolation.md) |
| ADR-016 | F-20 ledger sim-book — separate `sim_*` tables vs simMode flag | [accepted](ADR-016-ledger-sim-book-storage.md) |
| ADR-017 | F-20 VenueSimulator — new class vs reuse legacy `venue_sim_adapter` | [accepted](ADR-017-venue-simulator-vs-legacy-adapter.md) |
| ADR-018 | End-to-end design traceability chain | [accepted](ADR-018-design-traceability-chain.md) |
| ADR-019 | C4 documentation standard | [accepted](ADR-019-c4-documentation-standard.md) |
| ADR-020 | Event ordering, idempotency and delivery semantics | [accepted](ADR-020-event-ordering-idempotency.md) |
| ADR-021 | FlowOrder lifecycle and invariants | [accepted](ADR-021-floworder-lifecycle.md) |
| ADR-022 | Batch clearing solver and deterministic replay | [accepted](ADR-022-batch-clearing-solver-replay.md) |
| ADR-023 | LOB→FOB curve construction and validation | [accepted](ADR-023-lob-to-fob-curve.md) |
| ADR-024 | Time Alignment & Latency-Aware Venue Curve Correction | [proposed](ADR-024-latency-aware-venue-curve.md) |
| ADR-025 | Risk Manager boundaries and checks | [accepted](ADR-025-risk-manager-boundaries.md) |
| ADR-026 | Ledger accounting and PnL source of truth | [accepted](ADR-026-ledger-accounting-pnl.md) |
| ADR-027 | Execution routing algorithm strategy | [accepted](ADR-027-execution-routing-algorithm.md) |
| ADR-028 | Backtest/replay parity and audit mode | [accepted](ADR-028-backtest-replay-parity.md) |
| ADR-029 | LLM-assisted development governance | [accepted](ADR-029-llm-assisted-development-governance.md) |
| ADR-030 | Legacy MVP migration policy | [accepted](ADR-030-legacy-mvp-migration-policy.md) |
| ADR-031 | F-09 multi-leg execution modes & atomicity policies | [accepted](ADR-031-multileg-execution-modes-atomicity.md) |
| ADR-032 | F-09 parent/child (BatchOrder→ComboOrder→Leg) order model | [accepted](ADR-032-parent-child-order-model.md) |
| ADR-033 | F-09 `execution.groups` Kafka topic | [accepted](ADR-033-execution-groups-topic.md) |
| ADR-034 | F-09 grouped constraint solver (feasibility-gate now, OSQP QP later) | [accepted](ADR-034-grouped-constraint-solver.md) |
| ADR-035 | FOB Solver Mathematical Foundation (Hamiltonian-Based, IN-012) | [accepted](ADR-035-fob-solver-mathematical-foundation.md) |
| ADR-036 | F-09 atomic one-branch OCO via conditional branches (no new solver) | [accepted](ADR-036-atomic-one-branch-oco.md) |
| ADR-037 | F-09 external-leg execution + compensation (MVP-5, reuse F-12 hedge path) | [accepted](ADR-037-external-leg-execution-compensation.md) |
| ADR-038 | F-09 OCO/bracket runtime — leg-transition persistence + execution semantics (ранее ADR-035, переномерован из-за коллизии) | [accepted](ADR-038-oco-bracket-runtime.md) |
| ADR-039 | F-09 compensation resolution (MVP-6, operator-driven, no auto-cascade) | [accepted](ADR-039-compensation-resolution.md) |
| ADR-040 | F-09 compensation resolution — endpoint в order_flow, matching экспонирует CompensationService gRPC (MVP-6 slice 3b) | [accepted](ADR-040-compensation-resolution-cross-service.md) |
| ADR-041 | F-09 auto-policy compensation resolution (MVP-7, money-guardrails + circuit-breaker, default OFF) | [accepted](ADR-041-auto-policy-compensation-resolution.md) |
| ADR-042 | Единый read-слой батчей (single-leg + combo) через ClickHouse view | [accepted](ADR-042-unified-batch-read-layer.md) |
| ADR-043 | F-09 combo external-нога → HedgeFlow + ChildOrder (F-09↔F-12) | [accepted](ADR-043-combo-external-hedgeflow-childorder.md) |
| ADR-044 | F-06 модель балансов ledger — единый PG-источник vs dual in-memory + PG | [proposed](ADR-044-ledger-balance-source-of-truth.md) |
| ADR-045 | F-06 модель соединений с PostgreSQL — per-call vs in-app pool | [proposed](ADR-045-pg-connection-pooling.md) |
| ADR-046 | F-06 топик `positions.update` для WS-push обновлений позиций | [proposed](ADR-046-positions-update-topic.md) |
| ADR-047 | F-05A Surplus / EXCHANGE_PNL policy для vectorized external clearing | [accepted](ADR-047-surplus-exchange-pnl-policy.md) |
| ADR-048 | F-05A QP solver backend (OSQP) для vector clearing `Wx=0` | [accepted](ADR-048-qp-solver-backend.md) |
| ADR-049 | F-05A vector clearing execution → F-12 hedge (converged-only, flag) | [accepted](ADR-049-f05a-vector-clearing-execution-hedge.md) |

Шаблон ADR — в [../../../ЭТАПЫ.md](../../../ЭТАПЫ.md) §7 или [incoming-docs/2026-05-13-Этапы.md](../../../incoming-docs/2026-05-13-Этапы.md).
