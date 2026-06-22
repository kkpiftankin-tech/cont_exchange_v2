---
id: DOC-TEST-F-09
phase: 10-testing
status: draft
owner: core-team
source:
  - IN-011 §17 (Acceptance Criteria), §18 (Тестирование), §9 (Математика), §10 (Политики), §20 (Инварианты)
related:
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - docs/02-system/features/F-09-batch-combo-orders/acceptance-criteria.md
  - docs/05-components/sequences/SEQ-F09-UC-F09-01-services.md
  - docs/05-components/sequences/SEQ-F09-UC-F09-02-services.md
  - docs/05-components/sequences/SEQ-F09-UC-F09-03-services.md
  - docs/10-testing/features/F-04-test-plan.md
  - docs/10-testing/features/F-15-test-plan.md
  - ADR-031, ADR-032, ADR-033
---

# F-09 Batch / Combo / Multi-leg Orders — план тестирования

Полный спецификационный источник: [`incoming-docs/2026-06-05-F-09-batch-combo-orders-v2.md`](../../../incoming-docs/2026-06-05-F-09-batch-combo-orders-v2.md) §17–§18, §9–§10, §20.

> Легенда статуса: ✅ выполнено, ⚠ частично, ❌ не выполнено.
>
> F-09 в статусе `planned` — реализация не начата, все тесты ❌ (запланированы).

## Раздел 1. Unit-тесты (11 сценариев)

Чистая доменная и алгоритмическая логика без PostgreSQL/Kafka/живых venue. GTest. Файлы — `cpp/order_flow/tests/` (нормализация, OCO, bracket) и `cpp/matching/tests/` (solver, ratio, VWAP/IS).

| # | Тест | Цель / инвариант | AC |
| --- | --- | --- | --- |
| U-1 | NormalizeComboOrder | Нормализация `CreateComboOrder` → BatchOrder/ComboOrder + Leg[] + Constraint[] + ConditionalLink[]; `orchestration_only` даёт warning; невалидный instrument / sum(weights)≠1 → ошибка | AC-001, AC-009 |
| U-2 | BuildMultiLegVectorOrder | Построение `MultiLegVectorOrder`: signed leg vector, A_g/G_g, ρ_g, feasible caps; `Q_target=w·N/P_ref` | AC-001, AC-004 |
| U-3 | SolveGroupedBatchStrictAtomic | `α_g<α_min ⇒ α_g=0, fills=[], orphanLegs=0`; обе ноги ликвидны → filled | AC-003 |
| U-4 | SolveGroupedBatchScalableAtomic | `α_g*=min_ℓ(Q_feasible/Q_target)`, `e_g=α_g*·ρ_g`, ratioDeviation≤maxRatioDeviationBps | AC-004, AC-002 |
| U-5 | ApplyBestEffortPolicy | Фиксация `violatedConstraints`, `fallbackAction`, `ratioDeviationBps`, `degraded` | AC-005 |
| U-6 | RejectExternalStrictAtomicWithoutNativeSupport | `venue_native` без native поддержки → reject; `external_compensating ≠ strict_atomic` | AC-006 |
| U-7 | ApplyOCOTransitions | Исполнение ветви A идемпотентно отменяет siblings; дубль события не повторяет cancel | AC-007 |
| U-8 | ResizeBracketExits | `Q_tp=Q_sl=Q_entry_filled`; entry не исполнен → exits waiting | AC-008 |
| U-9 | CalculateRatioDeviation | bps-отклонение факт vs target; деление на 0 → нарушение | AC-004, AC-005 |
| U-10 | CalculateCombinedVWAP | `VWAP=Σp·q/Σq`; Decimal, не double | AC-009, AC-005 |
| U-11 | CalculateCombinedIS | `IS=Σ(p_exec−p_ref)·q·sign/Σq`; sell IS<0; stale ref → warning | AC-005, AC-009 |

Файлы: `normalize_combo_order_test.cpp`, `build_multileg_vector_order_test.cpp`, `solve_grouped_batch_strict_atomic_test.cpp`, `solve_grouped_batch_scalable_atomic_test.cpp`, `apply_best_effort_policy_test.cpp`, `reject_external_strict_atomic_test.cpp`, `apply_oco_transitions_test.cpp`, `resize_bracket_exits_test.cpp`, `calculate_ratio_deviation_test.cpp`, `calculate_combined_vwap_test.cpp`, `calculate_combined_is_test.cpp`.

## Раздел 2. Integration-тесты (11 сценариев)

Реальные PostgreSQL + Kafka (docker compose / testcontainers); venue — mock. Файлы `Testing/f09_*.sh` и `tests/integration/f09/`.

| # | Тест | Сценарий (по service sequences) | AC |
| --- | --- | --- | --- |
| IT-1 | ApiToLedgerGroupedExecution | `POST /v1/combo-orders` → order-flow → risk → PG → orders.normalized → matching → execution.groups+fills → ledger; idempotency на дубль | AC-001, AC-009 |
| IT-2 | PairStrictAtomicBothLegsLiquid | strict_atomic, обе ноги ликвидны → filled, orphanLegs=0 | AC-003, AC-004 |
| IT-3 | PairStrictAtomicOneLegIlliquid | strict_atomic, ETH вне диапазона → executionScale=0, fills=[], нет orphan | AC-003 |
| IT-4 | BasketScalableAtomicOneLegCapped | basket 3 ноги, SOL capped 50% → α_g*≈0.5, bindingLegs=[sol] | AC-004, AC-002 |
| IT-5 | BasketTargetWeightsDeviation | dev 3bps→filled; dev 8bps→degraded+violatedConstraints | AC-002, AC-005 |
| IT-6 | SpreadConstraintPassFail | `BTC−15·ETH` в диапазоне→исполнение; вне→waiting_next_batch, violated=[spread_range] | AC-001, AC-003 |
| IT-7 | ExternalLegBestEffort | external_compensating, partial fill → degraded, atomicityGuarantee=external_compensating | AC-005, AC-006 |
| IT-8 | ExternalStrictAtomicRejected | venue_native без поддержки → cancelled_by_atomicity + risk.alerts(GROUPED_ATOMICITY_FAILURE) | AC-006 |
| IT-9 | OCOBranchActivationAndCancel | ветвь A fill → B cancelled в PG; идемпотентно | AC-007 |
| IT-10 | BracketPartialEntryResize | entry 60% → TP/SL qMax=0.6 | AC-008 |
| IT-11 | ReplayDeterministicGroupedExecution | один вход дважды → идентичные ExecutionGroup/fills/status/diagnostics (кроме solveTimeMs) | AC-010 |

## Раздел 3. Negative-тесты (5 сценариев)

| # | Тест | Проверка | AC |
| --- | --- | --- | --- |
| N-1 | IndependentSplitCannotBeStrictAtomic | `orchestration_only`+`strict_atomic` → reject (INCOMPATIBLE_MODE_AND_POLICY); orders.normalized не публикуется | AC-001, AC-003 |
| N-2 | ExternalPartialFillCannotCommitAsAtomic | partial external fill → atomicityGuarantee≠strict_atomic | AC-006 |
| N-3 | MissingLegFillRejectStrictGroup | strict_atomic без fill обязательной ноги → CommitError, ExecutionGroup не записан | AC-003 |
| N-4 | DuplicateGroupEventNoDuplicateFills | дубль `execution.groups` → ledger idempotent по executionGroupId; нет повторных проводок | AC-007, AC-009 |
| N-5 | StaleReferencePriceNoSpreadExecution | устаревший P_ref → spread не исполняется, waiting_next_batch, metric stale_price_skip | AC-001, AC-006 |

## Раздел 4. Маппинг тестов → Acceptance Criteria

| AC | Тесты |
| --- | --- |
| AC-F09-001 | U-1, U-2, U-3a, U-4a, IT-1, IT-6, N-1, N-5 |
| AC-F09-002 | U-4d, U-9c, IT-4, IT-5 |
| AC-F09-003 | U-3b, U-3c, IT-2, IT-3, IT-6c, N-1, N-3 |
| AC-F09-004 | U-4, U-9, IT-4, IT-5a |
| AC-F09-005 | U-5, U-9b, U-11d, IT-5b, IT-7 |
| AC-F09-006 | U-6, IT-7, IT-8, N-2 |
| AC-F09-007 | U-7, IT-9, N-4 |
| AC-F09-008 | U-8, IT-10 |
| AC-F09-009 | U-10, U-11, IT-1, IT-4, N-4 |
| AC-F09-010 | IT-11 |

## Раздел 5. Расположение тестов

- **Unit (order-flow):** `cpp/order_flow/tests/{normalize_combo_order,apply_oco_transitions,resize_bracket_exits,validate_execution_mode_policy}_test.cpp`.
- **Unit (matching):** `cpp/matching/tests/{build_multileg_vector_order,solve_grouped_batch_strict_atomic,solve_grouped_batch_scalable_atomic,apply_best_effort_policy,reject_external_strict_atomic,calculate_ratio_deviation,calculate_combined_vwap,calculate_combined_is,commit_rule_strict_atomic,stale_reference_price}_test.cpp`.
- **Integration/E2E:** `Testing/f09_it{1..10}_*.sh` (docker-compose; VEA mock для IT-7/IT-8).
- **In-process:** `tests/integration/f09/{replay_deterministic,duplicate_event_idempotency}_test.cpp`.
- **Replay parity (IT-11):** стыкуется с F-15 harness как `grouped_replay_parity`; фикстуры `Testing/fixtures/f09/BatchInput_f09_v1.json`.

## Раздел 6. Детерминизм replay (AC-F09-010)

1. **UUID детерминизм:** `executionGroupId = UUID_v5(NAMESPACE_F09_EXEC_GROUP, batchId+parentOrderId)` (не random); аналог seed-детерминизма F-04 T-F04-102.
2. **Порядок обхода групп:** лексикографический по `parentOrderId`.
3. **Child graph transitions:** применяются в порядке `(parentOrderId, legId)`; без зависимости от wall clock / thread scheduling.
4. **Float-детерминизм:** все деньги — Decimal; `α_g*` через `min()` в фиксированном порядке индексов; `groupSolveTimeMs` исключён из сравнения.
5. **Config version:** replay требует фиксированного `solver_config.version`.

## Раздел 7. SLA grouped solve (расширение NFR-EXEC-002)

| Размер (групп × ног) | p50 groupSolveTimeMs | p95 |
| --- | --- | --- |
| ≤10 × 2 | ≤5 ms | ≤20 ms |
| 10–50 × 4 | ≤20 ms | ≤50 ms |
| 50–100 × 4 | ≤50 ms | ≤120 ms |

Замер — `ExecutionGroup.solverDiagnostics.groupSolveTimeMs`. Nightly критерий: p95 ≤ цель; доля groups с timeout-violation ≤ 1%. Числа — оценочные, подтверждаются профилированием при реализации (open question `solver-formulation-open`).

## Source Fragments

- IN-011 §9, §10, §17 (AC-F09-001..010), §18 (11 unit + 11 integration + 5 negative), §20
- SEQ-F09-UC-F09-01/02/03-services; F-04/F-15 test plans (шаблоны SLA и replay-детерминизма)
