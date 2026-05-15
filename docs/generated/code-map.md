# Code Map (auto-generated)

> Этот файл генерируется из specs/domain/code-map.yaml. Не редактируйте вручную.

| Component | Path | Role | Features | Proto contracts |
|---|---|---|---|---|
| cpp-common | `cpp/common` | shared-library | — | — |
| gateway | `cpp/gateway` | api-gateway | F-02, F-05, F-06 | order_flow_service.proto, orders.proto, common.proto |
| order-flow | `cpp/order_flow` | order-lifecycle | F-02, F-07 | orders.proto, order_flow_service.proto, risk.proto, ledger.proto, common.proto |
| matching | `cpp/matching` | batch-clearing | F-04 | orders.proto, batch.proto, common.proto |
| ledger | `cpp/ledger` | ledger-balances-reservations | F-04, F-06, F-12 | ledger.proto, batch.proto, execution.proto, common.proto |
| risk | `cpp/risk` | risk-manager | F-07 | risk.proto, common.proto |
| market-data | `cpp/market_data` | live-market-data | F-05 | marketdata_raw.proto, marketdata_raw.proto, common.proto |
| venues | `cpp/venues` | external-venues-adapter | F-05, F-11, F-12 | marketdata_raw.proto, execution.proto, common.proto |
| observability | `cpp/observability` | metrics-alerts-aggregator | F-04, F-05, F-07, F-11, F-12, F-15 | risk.proto, batch.proto, execution.proto, observability.proto |
| contracts-proto | `contracts/proto` | protobuf-source-of-truth | — | — |
| infra | `infra` | infrastructure-and-deployment | — | — |
| legacy-mvp | `legacy_mvp` | legacy-reference-and-migration-source | — | — |
