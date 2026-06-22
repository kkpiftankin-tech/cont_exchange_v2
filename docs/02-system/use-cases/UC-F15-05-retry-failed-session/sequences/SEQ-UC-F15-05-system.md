<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-UC-F15-05-system
level: kite
---
-->

# SEQ-UC-F15-05-system. Retry Failed Session: system view

## Type

System Context Sequence

## Feature

- [F-15](../../../features/F-15-backtest-replay/)

## Use Case

- [UC-F15-05](../use-case.md)

## Diagram

```mermaid
sequenceDiagram
    actor A as Analyst
    participant S as Continuous Exchange System

    A->>S: POST /api/v1/replay/sessions/{id}/retry (use_same_config, override?)
    S->>S: RBAC + ownership
    S->>S: Load original session (must be failed/cancelled)
    alt use_same_config = true
        S->>S: Copy snapshot 1:1
    else use_same_config = false
        S->>S: Build new snapshot from override_config
    end
    S->>S: Insert new session (retry_parent_id = original.id, status=pending)
    S-->>A: 201 Created (new session_id)
    Note over S: worker picks pending, runs same cycle as UC-F15-01
```
