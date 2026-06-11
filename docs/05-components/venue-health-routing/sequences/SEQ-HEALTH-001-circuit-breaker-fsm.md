<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-HEALTH-001-circuit-breaker-fsm
level: fish
component: venue-health-routing
---
-->

# SEQ-HEALTH-001. Circuit Breaker FSM

## Type

Internal Component Sequence (venue-health-routing)

## Feature

- [F-11](../../../02-system/features/F-11-external-venues-lob-to-fob/)

## Use Case

- [UC-F11-04](../../../02-system/use-cases/UC-F11-04-venue-health-degradation/use-case.md)

## Purpose

Внутренний FSM circuit breaker и реакция на новые RAW reports.

## Participants

- Service (app)
- VenueState (domain)
- CircuitBreaker (domain)
- KafkaProducer venue.health (AGGREGATED)

## Diagram

```mermaid
sequenceDiagram
    participant SVC as Service.OnRawReport
    participant ST as VenueState
    participant CB as CircuitBreaker
    participant PUB as Publisher

    SVC->>ST: OnRawReport(report)
    ST->>ST: append to sliding window
    ST->>CB: evaluate(window, now)

    alt errors >= threshold AND state == CLOSED
        CB->>CB: → OPEN (open_ts = now)
    else state == OPEN AND now - open_ts >= cooldown
        CB->>CB: → HALF_OPEN
    else state == HALF_OPEN AND probe ok
        CB->>CB: → CLOSED
    else state == HALF_OPEN AND probe fail
        CB->>CB: → OPEN (reset cooldown)
    end

    CB-->>ST: state
    ST->>ST: compute health_score + routing_recommendation
    SVC->>PUB: Publish(state) → AGGREGATED VenueHealth
```

## Related

- Service sequence: [SEQ-F11-04-health-routing-services](../../sequences/SEQ-F11-04-health-routing-services.md)
