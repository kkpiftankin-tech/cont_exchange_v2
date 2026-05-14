---
id: DOC-COMPONENTS-OVERVIEW
phase: 05-components
status: draft
owner: core-team
related:
  - docs/03-architecture/component-map.md
  - specs/domain/code-map.yaml
---

# Components Overview

| Component | Code | Role | Status |
| --- | --- | --- | --- |
| gateway | [cpp/gateway/](../../cpp/gateway/) | HTTP edge | active |
| order-flow | [cpp/order_flow/](../../cpp/order_flow/) | order lifecycle orchestrator | active |
| risk-manager | [cpp/risk/](../../cpp/risk/) | pre/post-trade risk | active-mvp |
| ledger | [cpp/ledger/](../../cpp/ledger/) | balances/reservations | active-mvp |
| matching-fob-core | [cpp/matching/](../../cpp/matching/) | batch clearing solver | mvp-simulator |
| market-data | [cpp/market_data/](../../cpp/market_data/) | tickers cache | active-mvp |
| external-venues | [cpp/venues/](../../cpp/venues/) | CEX/DEX adapter | mvp-simulator |
| observability-reporting | [cpp/observability/](../../cpp/observability/) | metrics/alerts | active-mvp |
| auth-identity | — | sessions/roles | not-implemented |
| backtest-replay | — | replay engine | not-implemented |

Связь с прото-контрактами и сущностями — в [../../specs/domain/code-map.yaml](../../specs/domain/code-map.yaml).
