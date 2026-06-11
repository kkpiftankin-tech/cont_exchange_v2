<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F09-02. Grouped matching внутри batch cycle

## Feature

- [F-09. Batch, Combo and Multi-leg Orders](../../features/F-09-batch-combo-orders/)

## Primary Actor

Matching Backend (backend-компонент сопоставления заявок)

## Supporting Actors

- Market Data Service (reference prices, spread inputs)
- Execution Planning (grouped preview, routing hints)
- Ledger (grouped postings)
- Risk Manager (post-trade grouped check)

## Preconditions

- Существуют active parent orders в `multileg_vector_solver`.
- Доступны reference prices для активов ног (F-05).

## Trigger

Очередной batch cycle (`batchIntervalMs`) клиринга.

## Main Flow

1. Matching загружает active parent orders и обновляет child graph
   (conditional / OCO / bracket / waiting branches).
2. Для каждой активной группы строит `MultiLegVectorOrder` (signed leg vector,
   target ratio/weights, constraint matrix, atomicity/fallback policy).
3. Получает reference prices и (если ноги допускают внешнее исполнение) venue
   curves + routing hints.
4. Вычисляет feasible caps по каждой ноге
   \(Q^{feasible}_{g,\ell} = \min(Q^{remaining}, Q^{rate}, Q^{liq}, Q^{risk}, Q^{venue})\).
5. Решает grouped problem: выбирает \(\alpha_g\) и \(e_g\), проверяет
   constraints (\(A_g e_g = b_g\alpha_g\), \(G_g e_g \le h_g\)).
6. Применяет policy post-processor (strict/scalable/best_effort) и commit rule
   (§11.4).
7. Формирует `ExecutionGroup` и leg fills.
8. Публикует `execution.groups` (+ per-leg `fills`).
9. Обновляет parent/leg statuses; выполняет child graph transitions (idempotent).
10. Ledger применяет grouped execution; Risk выполняет post-trade grouped check;
    Observability собирает grouped diagnostics.

## Alternative Flows

- **A1.** strict_atomic недоступна обязательная нога → `executionScale=0`,
  fills=[], group `cancelled_by_atomicity|waiting_next_batch`.
- **A2.** scalable_atomic ограничена ликвидностью/риском → общий масштаб
  уменьшается, ratio сохраняется в пределах tolerance.
- **A3.** Требуется внешнее исполнение ног → переход к
  [UC-F09-03](../UC-F09-03-external-leg-execution/use-case.md).

## Postconditions

- `execution_groups` + `group_state_transitions` обновлены.
- `LegFill` зафиксированы не раньше/одновременно с `ExecutionGroup` (§20).
- Ledger grouped postings применены идемпотентно.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F09-02-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F09-UC-F09-02-services.md)

## Related Contracts

- [execution.groups](../../../06-api/messaging/execution-groups.md)
- [orders.normalized](../../../06-api/messaging/orders-normalized.md)
- [MarketDataService/GetReferencePrices](../../../06-api/grpc/marketdata-get-reference-prices.md)

## Related Components

- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)
- [market-data](../../../05-components/market-data/overview.md)
- [execution-planning](../../../05-components/execution-planning/overview.md)
- [ledger](../../../05-components/ledger/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)

## Related Data

- [execution_groups, group_state_transitions, combo_order_legs](../../../07-data/oltp-schema.md)
- [grouped_execution_events, grouped_leg_fills, grouped_quality_metrics](../../../07-data/olap-schema.md)

## Acceptance Criteria

- [AC-F09-003](../../features/F-09-batch-combo-orders/acceptance-criteria.md#ac-f09-003-strict_atomic),
  [AC-F09-004](../../features/F-09-batch-combo-orders/acceptance-criteria.md#ac-f09-004-scalable_atomic),
  [AC-F09-009](../../features/F-09-batch-combo-orders/acceptance-criteria.md#ac-f09-009-ledger),
  [AC-F09-010](../../features/F-09-batch-combo-orders/acceptance-criteria.md#ac-f09-010-replay)

## Source

- IN-011 §7 UC-F09-002, §9, §11.3–§11.6, §16
