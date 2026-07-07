# Coverage Matrix

Tracks documentation coverage per feature: which artifacts of the traceability chain are present.

**Definition of "covered"**: see [feature-development-process.md §7.4.1](../00-methodology/feature-development-process.md) — three-tier model
(`covered-docs` ∧ `covered-code` ∧ `covered-runtime`) introduced by AUDIT-001 T-AUDIT-010.
Existing `Status` column aggregates the three tiers (mapping table in §7.4.2).

Index of features: [`docs/02-system/features/feature-index.md`](../02-system/features/feature-index.md).

## Legend

Cell values:

- ✅ — present
- ⚠️ — partial / stub
- ❌ — missing

Status values:

- `complete`
- `partial`
- `needs-contracts`
- `needs-data`
- `needs-sequences`
- `needs-tests`
- `blocked-conflict`

## Matrix

| Feature | Use Case | System Seq | Service Seq | Contracts | Data | Components | Tests | Status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| [F-01](../02-system/features/F-01-auth-and-identity/) | ✅ UC-F01-01 | ✅ | ✅ | ⚠️ TODO grpc/auth | ❌ users/sessions | ⚠️ auth-identity planned | ❌ | needs-contracts |
| [F-02](../02-system/features/F-02-create-floworder/) | ✅ UC-F02-01 | ✅ | ✅ | ✅ orders/risk/ledger gRPC + orders.normalized | ✅ flow_orders DDL | ✅ gateway/order-flow/risk/ledger | ⚠️ smoke | partial |
| [F-03](../02-system/features/F-03-order-lifecycle/) | ✅ UC-F03-01 | ✅ | ✅ | ⚠️ amend/cancel TODO | ✅ flow_orders DDL | ✅ | ❌ | needs-contracts |
| [F-04](../02-system/features/F-04-batch-clearing/) | ✅ UC-F04-01 (+alt flows) | ✅ | ✅ + ✅ internal SEQ-MATCHING-001 | ✅ batch.outputs (⚠️ split→fills planned, C-1) | ✅ fills/batch_results DDL | ✅ matching/ledger/risk/obs | ✅ test plan U1–U10 + SLA | partial (needs solver impl) |
| [F-05](../02-system/features/F-05-live-market-data/) | ✅ UC-F05-01 | ✅ | ✅ | ✅ marketdata.raw + grpc | ⚠️ Redis cache | ✅ | ❌ | needs-tests |
| [F-05A](../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md) | ⏳ UC-F05A-01..05 (planned) | ⏳ | ⏳ | ⏳ vector_liquidity.proto + marketdata.vectorized (planned) | ⏳ vector_flow_segments/vector_clearing_results/surplus_events (planned) | ⏳ market_data/matching/ledger | ⏳ test plan (T-F05A-5xx) | planned — IN-014 registered; feature.yaml + addendum ✅; ADR-044 (surplus) + ADR-045 (OSQP QP) proposed; docs-gate НЕ пройден (ingress-close pending), код после |
| [F-06](../02-system/features/F-06-positions-pnl-margin/) | ✅ UC-F06-01 | ✅ | ✅ | ⚠️ GetBalances/Snapshot TODO | ✅ positions/accounts DDL | ✅ | ❌ | needs-contracts |
| [F-07](../02-system/features/F-07-pretrade-risk/) | ✅ UC-F07-01 | ✅ | ✅ | ✅ risk gRPC + alerts | ✅ risk_limits DDL | ✅ | ⚠️ unit | partial |
| [F-08](../02-system/features/F-08-posttrade-risk-and-liquidations/) | ✅ UC-F08-01 | ✅ | ✅ | ✅ batch.outputs + alerts | ✅ positions/risk_snapshots DDL | ✅ | ❌ | needs-tests |
| [F-09](../02-system/features/F-09-batch-combo-orders/) | ✅ UC-F09-01/02/03 | ✅ | ✅ | ✅ combo + compensation gRPC + execution.groups/execution.intents (2 execution-planning контракта deferred — superseded by ADR-037) | ✅ combo_*/execution_groups/combo_compensations + grouped_* CH | ✅ order_flow/matching/venues/ledger/observability + frontend (combo + compensation pages) | ✅ unit/integration suites green (grouped_solver, constraint_evaluator, trigger_condition, child_graph + compensation repo live PG) + live E2E (basket scalable, compensation reject→resolve) | implemented — MVP-1…7 LIVE (PR #13): grouped solver→ledger, constraints, OCO/bracket+conditional, honest-mode, external legs→HedgeFlow/ChildOrder, compensation resolution (manual+auto policy), frontend (ADR-031…043). Остаток: QP re-solve (deferred ADR-034), retry_external auto, observability grouped_* CH dashboards |
| [F-10](../02-system/features/F-10-mm-curves/) | ✅ UC-F10-01 | ✅ | ✅ | ⚠️ UpsertCurve TODO | ⚠️ flow_orders+mm tag | ✅ | ❌ | needs-contracts |
| [F-11](../02-system/features/F-11-external-venues-lob-to-fob/) | ✅ UC-F11-01 | ✅ | ✅ | ✅ marketdata.raw + planned orders.normalized venue | ✅ marketdata CH DDL | ✅ | ❌ | needs-tests |
| [F-12](../02-system/features/F-12-execution-hedge/) | ✅ UC-F12-01 | ✅ | ✅ | ✅ execution.intents/reports | ✅ execution_reports CH DDL | ✅ | ❌ | needs-tests |
| [F-13](../02-system/features/F-13-posttrade-report/) | ✅ UC-F13-01 | ✅ | ✅ | ⚠️ GET /reports TODO | ✅ fills/batch_results CH DDL | ✅ | ❌ | needs-contracts |
| [F-14](../02-system/features/F-14-deposit-withdraw/) | ✅ UC-F14-01 | ✅ | ✅ | ⚠️ deposit endpoints TODO | ✅ collateral_transfers DDL | ⚠️ custody-adapter planned | ❌ | needs-contracts |
| [F-15](../02-system/features/F-15-backtest-replay/) | ✅ UC-F15-01 | ✅ | ✅ | ✅ replay.proto + orders/batch archive + Kafka replay.{commands,results} | ✅ PG replay schemas + CH read-only | ✅ cpp/backtest (in-progress-impl) | ⚠️ unit only | in-progress |
| [F-16](../02-system/features/F-16-operator-console/) | ✅ UC-F16-01 | ✅ | ✅ | ⚠️ kill-switch endpoint TODO | ⚠️ audit log TODO | ✅ | ❌ | needs-contracts |
| [F-17](../02-system/features/F-17-monitoring-and-alerts/) | ✅ UC-F17-01 | ✅ | ✅ | ✅ alerts + observability | ✅ risk_events CH DDL | ✅ observability-reporting | ❌ | needs-tests |
