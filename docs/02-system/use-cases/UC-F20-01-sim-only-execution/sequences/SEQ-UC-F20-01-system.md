# SEQ-UC-F20-01-system — SIM_ONLY execution (system-level)

- Feature: [F-20](../../../features/F-20-live-venue-simulator/README.md)
- Use case: [UC-F20-01](../use-case.md)
- Level: L0 (внешние актёры ↔ Continuous Exchange System как чёрный ящик)

Внешняя площадка выступает только источником **живого** LOB; ордера на неё не уходят
(SIM_ONLY). Оператор задаёт sim-режим; система возвращает синтетическое исполнение.

```mermaid
sequenceDiagram
    actor OP as Оператор
    participant SYS as Continuous Exchange System
    participant VENUE as Внешняя площадка (только LOB)

    Note over VENUE,SYS: Боевой приём живого стакана
    loop Живой LOB
        VENUE-->>SYS: LOB update
    end

    OP->>SYS: Создать SimSession (SIM_ONLY, venue+symbol, модели)
    SYS-->>OP: simSessionId, status=ACTIVE

    Note over SYS: Внутри системы F-12 инициирует child-ордер
    SYS->>SYS: ChildOrderRequest → SIM_ONLY → симуляция на живом LOB
    alt LOB свежий (lobAge < порог)
        SYS-->>OP: SimExecutionReport (simMode=true, filled/partial), sim-книга обновлена
    else Stale LOB / нет ликвидности / reject
        SYS-->>OP: SimExecutionReport (REJECTED, SIM_STALE_LOB / SIM_NO_LIQUIDITY / …) + алерт
    end
```
