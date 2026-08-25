<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-UC-F15-04-system
level: kite
---
-->

# SEQ-UC-F15-04-system. Audit-mode Replay: system view

## Type

System Context Sequence

## Feature

- [F-15](../../../features/F-15-backtest-replay/)

## Use Case

- [UC-F15-04](../use-case.md)

## Diagram

```mermaid
sequenceDiagram
    actor U as Auditor
    participant S as Continuous Exchange System

    U->>S: POST /api/v1/replay/audit-runs (batch_id, tolerance)
    S->>S: RBAC (admin + replay:execute)
    S->>S: Load production BatchResult + flow_orders snapshot + marketdata
    S->>S: Restore snapshot configs at config_version
    S->>S: Replay one batch (isolated solver call)
    S->>S: Compute diff vs production (clear_prices, fills, residual_norm)
    S->>S: equivalent := IsEquivalent(diff, tolerance)
    S-->>U: 200 OK (audit_run_id, equivalent, diff)
```

## Related Service Sequence

- [SEQ-F15-04-audit-mode-services](../../../../05-components/sequences/SEQ-F15-04-audit-mode-services.md)
