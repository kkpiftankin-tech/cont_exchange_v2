<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-UC-F09-03-system
level: kite
---
-->

# SEQ-UC-F09-03-system. External Leg Execution / Compensating: system view

## Type

System Context Sequence

## Feature

- [F-09. Batch, Combo and Multi-leg Orders](../../../features/F-09-batch-combo-orders/)

## Use Case

- [UC-F09-03. External leg execution / compensating](../use-case.md)

## Purpose

Показывает внешним акторам (Trader и External Venue) взаимодействие с Continuous Exchange System при маршрутизации ног многоногой заявки на внешние площадки. Ветвление по `atomicityScope` (internal_batch / venue_native / external_compensating) и возможные исходы: заполнение, деградация, компенсация.

Continuous Exchange System — непрозрачный блок. Внутренние компоненты (execution-planning, venue-execution-adapter) не видны — виден только External Venue как внешний участник.

## Participants

- Trader / API Client
- Continuous Exchange System
- External Venue

## Diagram

```mermaid
sequenceDiagram
    actor T as Trader / API Client
    participant S as Continuous Exchange System
    participant V as External Venue

    note over T,S: Триггер: grouped solve определил ноги, требующие внешней ликвидности

    alt atomicityScope = internal_batch
        note over S: Внешнее исполнение обязательных ног ЗАПРЕЩЕНО
        S-->>T: ExecutionGroupUpdate(groupStatus=waiting_next_batch\n|cancelled_by_atomicity,\nreason=external_legs_forbidden_for_internal_scope)
    else atomicityScope = venue_native
        S->>V: MultiLegOrderRequest(legs[], nativeAtomic=true)
        alt venue поддерживает native atomic multi-leg
            V-->>S: MultiLegFillReport(execQty[], execPrice[], atomicGuarantee=true)
            S-->>T: ExecutionGroupUpdate(groupStatus=filled|partial,\natomicityGuarantee=venue_native)
            S-->>T: LegFillNotification(legId, execQty, execPrice) [per leg]
        else venue не поддерживает / таймаут
            V-->>S: Reject / Timeout
            S-->>T: ExecutionGroupUpdate(groupStatus=cancelled_by_atomicity,\nreason=venue_no_native_atomic_support)
        end
    else atomicityScope = external_compensating
        S->>V: LegOrder(leg_1) [best effort]
        V-->>S: PartialFillReport(leg_1)
        S->>V: LegOrder(leg_2) [best effort]
        alt leg_2 исполнена — целевая структура сохранена
            V-->>S: FillReport(leg_2)
            S-->>T: ExecutionGroupUpdate(groupStatus=filled|partial,\natomicityGuarantee=external_compensating)
            S-->>T: LegFillNotification(legId, execQty) [per leg]
        else leg_2 отклонена / таймаут → нарушение структуры
            V-->>S: Reject / Timeout (leg_2)
            note over S: Запускается compensating action
            S-->>T: ExecutionGroupUpdate(groupStatus=compensating\n|rollback_pending|degraded,\nviolatedConstraints[], fallbackAction)
            S-->>T: RiskAlert(EXTERNAL_COMPENSATION_REQUIRED,\nparentOrderId, affectedLegs[])
        end
    end

    note over T,S: Статус группы и ног ВСЕГДА отражает реальную гарантию,\nникогда не маркируется strict_atomic без venue_native
```

## Related Service Sequence

- [SEQ-F09-UC-F09-03-services](../../../../05-components/sequences/SEQ-F09-UC-F09-03-services.md)

## Related Feature

- [F-09](../../../features/F-09-batch-combo-orders/)

## Related ADR

- [ADR-031](../../../../03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md) — atomicityScope и политики
- [ADR-033](../../../../03-architecture/adr/ADR-033-execution-groups-topic.md)

## Source

- IN-011 §7 UC-F09-003, §2.2, §10.5, §12.7, §12.8, §15.3
