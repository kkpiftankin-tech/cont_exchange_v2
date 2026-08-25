# SEQ-UC-F20-02-system — SHADOW compare (system-level)

- Feature: [F-20](../../../features/F-20-live-venue-simulator/README.md)
- Use case: [UC-F20-02](../use-case.md)
- Level: L0

```mermaid
sequenceDiagram
    actor OP as Оператор/инженер
    participant SYS as Continuous Exchange System
    participant VENUE as Внешняя площадка (LIVE-ордер)

    Note over SYS: routingMode=SHADOW активен
    SYS->>SYS: ChildOrderRequest → fork LIVE + SIM
    SYS->>VENUE: LIVE-ордер (реальное исполнение)
    VENUE-->>SYS: ExecutionReport (simMode=false)
    SYS->>SYS: Симуляция на живом LOB → SimExecutionReport (simMode=true)
    SYS->>SYS: Divergence: delta fillRate / price / latency / fee по clientOrderId
    SYS-->>OP: Сравнение SIM vs LIVE; алерт при divergence > threshold
```
