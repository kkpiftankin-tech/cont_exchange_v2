<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-UC-F09-01-system
level: kite
---
-->

# SEQ-UC-F09-01-system. Create Combo / Batch Order: system view

## Type

System Context Sequence

## Feature

- [F-09. Batch, Combo and Multi-leg Orders](../../../features/F-09-batch-combo-orders/)

## Use Case

- [UC-F09-01. Создать combo / multi-leg order](../use-case.md)

## Purpose

Показывает взаимодействие внешнего актора (Trader / API Client) с Continuous Exchange System как единым непрозрачным блоком при создании ComboOrder или BatchOrder. Внутренние сервисы (order-flow, risk-manager, matching, ledger) не видны — только наблюдаемые внешне события.

Сценарий охватывает:
- создание заявки в режиме `orchestration_only` (предупреждение о независимых ногах);
- создание заявки в режиме `multileg_vector_solver` (grouped execution guarantees);
- опциональный grouped preview перед подачей;
- ветки rejection и throttle.

## Participants

- Trader / API Client
- Continuous Exchange System

## Diagram

```mermaid
sequenceDiagram
    actor T as Trader / API Client
    participant S as Continuous Exchange System

    note over T,S: Опционально: grouped preview перед созданием заявки
    T->>S: PreviewComboOrder(legs, constraints, executionMode)
    S-->>T: GroupedPreview(expectedScale, expectedLegFills,\ncombinedVWAP, ratioDeviation, bindingLeg,\nbindingConstraints, externalExecRisk)

    note over T,S: Создание заявки — orchestration_only или multileg_vector_solver
    T->>S: CreateComboOrder(legs, constraints, graphLinks,\nexecutionMode, atomicityPolicy, atomicityScope,\nclientComboId)

    alt executionMode = orchestration_only
        S-->>T: WARNING: ноги исполняются независимо;\nвеса / ratio / spread / portfolio exposure\nмогут отклониться от целевых значений
        S-->>T: ParentOrderAccepted(parentOrderId, status=active,\nlegOrderIds[])
    else executionMode = multileg_vector_solver, risk approved
        S-->>T: ParentOrderAccepted(parentOrderId,\nstatus=active|waiting_for_trigger,\nexecutionMode, atomicityPolicy)
    else risk rejected
        S-->>T: ParentOrderRejected(parentOrderId,\nrejectReason, violatedChecks[])
    else risk throttled / resize
        S-->>T: ParentOrderThrottled(parentOrderId,\nstatus=throttled, adjustedParams)
    end

    note over T,S: После создания — обновления статусов (push / poll)
    S-->>T: LegStatusUpdate(legId, status=active|waiting_for_trigger)
    S-->>T: ParentStatusUpdate(parentOrderId, status=active)
```

## Related Service Sequence

- [SEQ-F09-UC-F09-01-services](../../../../05-components/sequences/SEQ-F09-UC-F09-01-services.md)

## Related Feature

- [F-09](../../../features/F-09-batch-combo-orders/)

## Related ADR

- [ADR-031](../../../../03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md) — два режима исполнения и пять политик атомарности
- [ADR-032](../../../../03-architecture/adr/ADR-032-parent-child-order-model.md) — parent/child модель заявок

## Source

- IN-011 §7 UC-F09-001, §11.1, §11.2, §16
