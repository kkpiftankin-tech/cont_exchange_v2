# SEQ-UC-F11-04-system. Venue health degradation: system view

## Type

System Context Sequence

## Feature

- [F-11](../../../features/F-11-external-venues-lob-to-fob/)

## Use Case

- [UC-F11-04](../use-case.md)

## Purpose

Показать, что видят внешние участники при деградации площадки: оператор получает alert, трейдеры видят индикатор и автоматическое переключение routing'а. Continuous Exchange System внутри сама детектирует и реагирует.

## Participants

- Внешняя площадка
- Continuous Exchange System
- Operator (получатель alert'а)
- Trader (наблюдает routing)

## Diagram

```mermaid
sequenceDiagram
    participant V as External Venue
    participant S as Continuous Exchange System
    actor OP as Operator
    actor T as Trader

    alt stale > threshold
        Note over S: timer detect stale
        S->>S: VenueHealth status=STALE
    else errors > CIRCUIT_BREAKER_ERRORS
        V--xS: errors / timeouts
        S->>S: CB CLOSED → OPEN
        S->>S: VenueHealth status=DISCONNECTED, routing=BLOCK
    end

    S-->>OP: alert + dashboard update
    S-->>T: routing переключился на другую venue

    Note over S: cooldown
    S->>S: CB OPEN → HALF_OPEN
    S->>V: probe
    alt probe ok
        V-->>S: ok
        S->>S: CB → CLOSED, routing=ALLOW
        S-->>OP: alert resolved
    else probe fail
        V--xS: error
        S->>S: CB → OPEN (stay blocked)
    end
```

## Related Service Sequence

- [SEQ-F11-04-health-routing-services](../../../../05-components/sequences/SEQ-F11-04-health-routing-services.md)
