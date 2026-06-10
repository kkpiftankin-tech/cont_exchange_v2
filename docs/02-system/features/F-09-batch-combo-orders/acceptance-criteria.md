# F-09 — Acceptance Criteria

Источник: IN-011 (F-09 v2 corrected) §17. Каждый критерий ссылается на тесты из
[F-09 test plan](../../../10-testing/features/F-09-test-plan.md).

## Функциональные критерии

### AC-F09-001. Настоящий multi-leg solver

Если `executionMode = multileg_vector_solver`, Matching Backend **не имеет
права** исполнять ноги как независимые FlowOrder без общего grouped solve.
Результат — один `ExecutionGroup` с вектором \(e_g\).
Тесты: `SolveGroupedBatchScalableAtomic`, `IndependentSplitCannotBeStrictAtomic`.

### AC-F09-002. Защита от перекоса (portfolio skew)

Для `BasketOrder` с жёсткими `target weights` итоговое исполнение не выводит
портфель за `maxWeightDeviationBps` (по умолч. 5 bps).
Тесты: `BasketTargetWeightsDeviation`, `BasketScalableAtomicOneLegCapped`.

### AC-F09-003. strict_atomic

При недоступности хотя бы одной обязательной ноги:
`executionScale = 0`, `fills = []`,
`groupStatus ∈ {cancelled_by_atomicity, waiting_next_batch}`, `orphanLegs = 0`.
Тесты: `SolveGroupedBatchStrictAtomic`, `MissingLegFillRejectStrictGroup`,
`PairStrictAtomicOneLegIlliquid`.

### AC-F09-004. scalable_atomic

При ограничении одной ноги все ноги исполняются с общим уменьшенным масштабом
\(e_g = \alpha_g \rho_g\); отклонение ratio ≤ tolerance.
Тесты: `SolveGroupedBatchScalableAtomic`, `BasketScalableAtomicOneLegCapped`.

### AC-F09-005. best_effort

Система обязана записать: `violatedConstraints`, `fallbackAction`,
`ratioDeviation`, user-visible `degraded` status.
Тесты: `ApplyBestEffortPolicy`, `ExternalLegBestEffort`.

### AC-F09-006. External execution

Если ноги идут на внешние площадки без native atomic support, результат **не**
маркируется `strict_atomic`.
Тесты: `RejectExternalStrictAtomicWithoutNativeSupport`,
`ExternalStrictAtomicRejected`, `ExternalPartialFillCannotCommitAsAtomic`.

### AC-F09-007. OCO

Исполнение одной OCO-ветви **идемпотентно** отменяет все sibling-ветви.
Тесты: `ApplyOCOTransitions`, `OCOBranchActivationAndCancel`,
`DuplicateGroupEventNoDuplicateFills`.

### AC-F09-008. Bracket order

Exit-ветви активируются только на фактически исполненный `entry volume`
(\(Q_{tp} = Q_{sl} = Q_{entry}^{filled}\)).
Тесты: `ResizeBracketExits`, `BracketPartialEntryResize`.

### AC-F09-009. Ledger

Ledger применяет grouped execution: по каждой ноге, по parent order, по fees,
по PnL, по margin impact. Идемпотентность по `(executionGroupId, legId)`.
Тесты: `ApiToLedgerGroupedExecution`, `DuplicateGroupEventNoDuplicateFills`.

### AC-F09-010. Replay

История grouped execution детерминированно воспроизводится в F-15 Replay с тем
же `ExecutionGroup`, leg fills, status transitions, solver diagnostics при
одинаковом входе.
Тесты: `ReplayDeterministicGroupedExecution`.

### AC-F09-011. Honest-mode гарантии (F-09 v2 system-impact §9)

API-ответ создания combo и UI **обязаны** показывать `executionMode` и
**фактические гарантии** — нельзя выдавать `orchestration_only` за настоящий
multi-leg. `CreateComboOrderResponse` несёт `execution_guarantees`
(human-readable) и `ratio_guaranteed` (флаг): `orchestration_only` →
`ratio_guaranteed=false` (нет гарантии ratio/weights/spread);
`multileg_vector_solver` → `true` (в пределах policy/tolerance);
`external_compensating` → `false` (не атомарно). **Реализовано** в
`CreateComboOrderUseCase`. Тест: `order_flow_combo_uc_tests` (orchestration_only
→ ratio NOT guaranteed).

## Итоговое правило (gate)

F-09 считается реализованной корректно только если поддержаны **оба** режима
(`orchestration_only` и `multileg_vector_solver`), и любая заявка с сохранением
weights/ratio/spread/factor/budget/leverage/margin/portfolio-risk идёт через
`multileg_vector_solver` (IN-011 §21).

## Трассируемость

| AC | Источник (IN-011) | Tests | Tasks |
| --- | --- | --- | --- |
| AC-F09-001 | §17 AC-1, §2.2, §3, §21 | unit+neg | T-F09-040..043 |
| AC-F09-002 | §17 AC-2, §9.2 | int | T-F09-041 |
| AC-F09-003 | §17 AC-3, §10.1 | unit+int+neg | T-F09-044 |
| AC-F09-004 | §17 AC-4, §10.2 | unit+int | T-F09-044 |
| AC-F09-005 | §17 AC-5, §10.3 | unit+int | T-F09-044 |
| AC-F09-006 | §17 AC-6, §10.5, §12.8 | unit+int+neg | T-F09-050..052 |
| AC-F09-007 | §17 AC-7, §11.6 | unit+int+neg | T-F09-045 |
| AC-F09-008 | §17 AC-8, §11.6 | unit+int | T-F09-045 |
| AC-F09-009 | §17 AC-9, §11.5, §12.9 | int | T-F09-060..061 |
| AC-F09-010 | §17 AC-10, §3 | int | T-F09-046, F-15 parity |
| AC-F09-011 | F-09 v2 system-impact §9 | unit | order_flow combo create (honest-mode) |
