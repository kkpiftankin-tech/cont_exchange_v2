<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-UC-F11-01-system
level: kite
---
-->

# SEQ-UC-F11-01-system. Onboard venue: system view

## Type

System Context Sequence

## Feature

- [F-11](../../../features/F-11-external-venues-lob-to-fob/)

## Use Case

- [UC-F11-01](../use-case.md)

## Purpose

Показать «чёрный ящик» онбординга площадки: оператор подаёт конфиг → Continuous Exchange System делает test-then-commit и начинает публиковать market-data + health.

## Participants

- Operator (актор)
- Continuous Exchange System
- Внешняя площадка (CEX / DEX / AMM)
- Trader (косвенный наблюдатель — видит новую площадку в UI после активации)

## Diagram

```mermaid
sequenceDiagram
    actor OP as Operator
    participant S as Continuous Exchange System
    participant V as External Venue (CEX/DEX/AMM)
    actor T as Trader

    OP->>S: POST /api/v1/venues (is_active=false)
    S-->>OP: 200 OK { venue_config }

    OP->>S: POST /api/v1/venues/{id}/reconnect
    S->>V: connect (WS/REST/RPC)
    V-->>S: handshake / first message
    S-->>OP: heartbeat { status, latency_ms, health_score }

    OP->>S: POST /api/v1/venues/{id}/enable
    S-->>OP: 200 OK { is_active=true }

    Note over S,V: continuous market data
    V-->>S: order book / trades
    S-->>T: новая площадка доступна в UI и в matching
```

## Related Service Sequence

- [SEQ-F11-01-onboard-venue-services](../../../../05-components/sequences/SEQ-F11-01-onboard-venue-services.md)
