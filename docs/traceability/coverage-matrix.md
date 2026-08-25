# Coverage Matrix

Tracks documentation coverage per feature: which artifacts of the traceability chain are present.

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
| [F-06](../02-system/features/F-06-positions-pnl-margin/) | ✅ UC-F06-01 | ✅ | ✅ | ⚠️ GetBalances/Snapshot TODO | ✅ positions/accounts DDL | ✅ | ❌ | needs-contracts |
| [F-07](../02-system/features/F-07-pretrade-risk/) | ✅ UC-F07-01 | ✅ | ✅ | ✅ risk gRPC + alerts | ✅ risk_limits DDL | ✅ | ⚠️ unit | partial |
| [F-08](../02-system/features/F-08-posttrade-risk-and-liquidations/) | ✅ UC-F08-01 | ✅ | ✅ | ✅ batch.outputs + alerts | ✅ positions/risk_snapshots DDL | ✅ | ❌ | needs-tests |
| [F-09](../02-system/features/F-09-batch-combo-orders/) | ✅ UC-F09-01 | ✅ | ✅ | ⚠️ ComboOrder TODO | ⚠️ flow_orders+combo_id | ✅ | ❌ | needs-contracts |
| [F-10](../02-system/features/F-10-mm-curves/) | ✅ UC-F10-01 | ✅ | ✅ | ⚠️ UpsertCurve TODO | ⚠️ flow_orders+mm tag | ✅ | ❌ | needs-contracts |
| [F-11](../02-system/features/F-11-external-venues-lob-to-fob/) | ✅ UC-F11-01 | ✅ | ✅ | ✅ marketdata.raw + planned orders.normalized venue | ✅ marketdata CH DDL | ✅ | ❌ | needs-tests |
| [F-12](../02-system/features/F-12-execution-hedge/) | ✅ UC-F12-01 | ✅ | ✅ | ✅ execution.intents/reports | ✅ execution_reports CH DDL | ✅ | ❌ | needs-tests |
| [F-13](../02-system/features/F-13-posttrade-report/) | ✅ UC-F13-01 | ✅ | ✅ | ⚠️ GET /reports TODO | ✅ fills/batch_results CH DDL | ✅ | ❌ | needs-contracts |
| [F-14](../02-system/features/F-14-deposit-withdraw/) | ✅ UC-F14-01 | ✅ | ✅ | ⚠️ deposit endpoints TODO | ✅ collateral_transfers DDL | ⚠️ custody-adapter planned | ❌ | needs-contracts |
| [F-15](../02-system/features/F-15-backtest-replay/) | ✅ UC-F15-01 | ✅ | ✅ | ✅ orders.normalized/batch.outputs archive | ✅ agent_logs CH DDL | ⚠️ backtest-replay planned | ❌ | needs-data |
| [F-16](../02-system/features/F-16-operator-console/) | ✅ UC-F16-01 | ✅ | ✅ | ⚠️ kill-switch endpoint TODO | ⚠️ audit log TODO | ✅ | ❌ | needs-contracts |
| [F-17](../02-system/features/F-17-monitoring-and-alerts/) | ✅ UC-F17-01 | ✅ | ✅ | ✅ alerts + observability | ✅ risk_events CH DDL | ✅ observability-reporting | ❌ | needs-tests |
