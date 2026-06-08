# UC-F09-03. External leg execution / compensating

## Feature

- [F-09. Batch, Combo and Multi-leg Orders](../../features/F-09-batch-combo-orders/)

## Primary Actor

Execution Planning / Venue Execution Adapter

## Supporting Actors

- Matching Backend (определяет, что часть ног требует внешней ликвидности)
- Ledger (compensation postings)

## Preconditions

- Группа в `multileg_vector_solver`; часть legs допускает/требует внешнее
  исполнение (venuePreferences != only internal).
- Задан `atomicityScope` (`internal_batch` / `venue_native` /
  `external_compensating`).

## Trigger

В grouped solve ([UC-F09-02](../UC-F09-02-grouped-matching/use-case.md))
Matching определяет ноги, требующие внешней ликвидности.

## Main Flow

1. Execution Planning строит grouped routing hints.
2. Ветвление по `atomicityScope`:
   - `internal_batch` — внешнее исполнение **обязательных** ног **запрещено**.
   - `venue_native` — заявка может быть отправлена только на venue с native
     atomic multi-leg order.
   - `external_compensating` — система исполняет ноги в режиме best effort и при
     частичном результате запускает compensating action.
3. Venue Execution Adapter исполняет внешние ноги, если policy разрешает; **не**
   заявляет `strict_atomic` без native support (AC-F09-006).
4. `ExecutionGroup` получает точный статус: `filled` / `partial` / `degraded` /
   `compensating` / `rollback_pending` / `rolledback`.
5. Ledger применяет compensation postings; интерфейс и отчёт явно показывают,
   что это **не** атомарное исполнение.

## Alternative Flows

- **A1.** Внешняя площадка отвергла/таймаут → нога `failed_external`;
  группа `compensating`/`rollback_pending`.
- **A2.** `strict_atomic` запрошен для внешних ног без `venue_native` → заявка
  отклонена ещё на pre-trade (риск/order_flow), либо переведена в допустимую
  политику только с явного разрешения (`sequential_fallback`).

## Postconditions

- Статус группы и ног отражает реальную (не атомарную) гарантию.
- `risk.alerts`: `EXTERNAL_COMPENSATION_REQUIRED` при необходимости компенсации.
- Никакого «отката уже совершённой внешней сделки» (вне scope MVP) — только
  compensating action.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F09-03-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F09-UC-F09-03-services.md)

## Related Contracts

- [execution.intents / execution.venue](../../../06-api/messaging/execution-topics.md)
- [execution.groups](../../../06-api/messaging/execution-groups.md)
- [risk.alerts](../../../06-api/messaging/topics.md)

## Related Components

- [execution-planning](../../../05-components/execution-planning/overview.md)
- [venue-execution-adapter](../../../05-components/venue-execution-adapter/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)
- [ledger](../../../05-components/ledger/overview.md)

## Related Data

- [execution_groups, group_state_transitions](../../../07-data/oltp-schema.md)

## Acceptance Criteria

- [AC-F09-006](../../features/F-09-batch-combo-orders/acceptance-criteria.md#ac-f09-006-external-execution)

## Source

- IN-011 §7 UC-F09-003, §10.5, §12.7, §12.8, §15.3
