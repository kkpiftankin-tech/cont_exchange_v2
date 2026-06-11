# Trace Matrix: L2 Sequence → Implementation Code

> **Ось:** реализационная (IN-013).
> **Уровни:** L2 🐟 Component-internal sequence → конкретные `cpp/<component>/src/...` файлы.
>
> Эта матрица показывает: **какие исходные файлы реализуют каждую L2 sequence**.
>
> Полная методология — [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../00-methodology/functional-hierarchy-and-decomposition.md).

## Mapping convention

L2 sequence frontmatter должен содержать `implemented-by:` (список путей)
и `tested-by:` (список тестов). Эта матрица — материализация всех таких
ссылок в один обзор.

## Текущее покрытие

> **Статус**: систематические L2 sequences ещё не построены для большинства
> компонентов (см. OQ-IN013-03 в [uc-to-sequences.md](uc-to-sequences.md)).
> Ниже — implementations, на которые ссылается доменная документация и
> IN-012 (ADR-035 / solver-foundation.md).

### Matching service (`cpp/matching/`)

| L2 sequence (TODO) | Implementation | Reference |
| --- | --- | --- |
| `SEQ-MATCHING-NNN-solve-batch` (TBD) | [cpp/matching/src/domain/solver_impl.cpp](../../cpp/matching/src/domain/solver_impl.cpp) | IN-012 §4, ADR-035, [solver-foundation.md](../09-implementation/solver-foundation.md) |
| `SEQ-MATCHING-NNN-grouped-solve` (TBD) | [cpp/matching/src/domain/grouped_solver_bisection.cpp](../../cpp/matching/src/domain/grouped_solver_bisection.cpp), [multileg_feasible_caps.cpp](../../cpp/matching/src/domain/multileg_feasible_caps.cpp) | ADR-034, F-09 |
| `SEQ-MATCHING-NNN-batch-cycle` (TBD) | [cpp/matching/src/app/run_batch_uc.cpp](../../cpp/matching/src/app/run_batch_uc.cpp), [matching_loop.cpp](../../cpp/matching/src/app/matching_loop.cpp) | F-04 |
| `SEQ-MATCHING-NNN-hedge-trigger` (TBD) | [cpp/matching/src/app/hedge_trigger_policy.cpp](../../cpp/matching/src/app/hedge_trigger_policy.cpp), [execution_planner.cpp](../../cpp/matching/src/app/execution_planner.cpp), [execution_intent_builder.cpp](../../cpp/matching/src/app/execution_intent_builder.cpp) | F-12 |

### Venues service (`cpp/venues/`)

| L2 sequence (TODO) | Implementation | Reference |
| --- | --- | --- |
| `SEQ-VENUES-NNN-depth-curve` (TBD) | [cpp/venues/src/domain/depth_curve_builder.cpp](../../cpp/venues/src/domain/depth_curve_builder.cpp) | F-11, ADR-023 |
| `SEQ-VENUES-NNN-cex-adapter` (TBD) | [cpp/venues/src/infra/cex_ws_rest_adapter.cpp](../../cpp/venues/src/infra/cex_ws_rest_adapter.cpp) | F-11 (circuit-breaker FSM) |
| `SEQ-VENUES-NNN-venues-loop` (TBD) | [cpp/venues/src/app/venues_loop.cpp](../../cpp/venues/src/app/venues_loop.cpp) | F-11/F-12/F-20 (ADR-015 isolation) |

### Order Flow service (`cpp/order_flow/`)

| L2 sequence (TODO) | Implementation | Reference |
| --- | --- | --- |
| `SEQ-ORDERFLOW-NNN-create-flow-order` (TBD) | [cpp/order_flow/src/app/order_flow_uc.cpp](../../cpp/order_flow/src/app/order_flow_uc.cpp) | F-02 |
| `SEQ-ORDERFLOW-NNN-create-combo-order` (TBD) | [cpp/order_flow/src/app/create_combo_order_use_case.cpp](../../cpp/order_flow/src/app/create_combo_order_use_case.cpp), [cancel_combo_order_use_case.cpp](../../cpp/order_flow/src/app/cancel_combo_order_use_case.cpp) | F-09 |
| `SEQ-ORDERFLOW-NNN-resolve-compensation` (TBD) | [cpp/order_flow/src/app/resolve_compensation_use_case.cpp](../../cpp/order_flow/src/app/resolve_compensation_use_case.cpp) | F-09 MVP-6 (ADR-039) |

### Risk service (`cpp/risk/`)

| L2 sequence (TODO) | Implementation | Reference |
| --- | --- | --- |
| `SEQ-RISK-NNN-pretrade-check` (TBD) | [cpp/risk/src/transport/grpc_risk_service.cpp](../../cpp/risk/src/transport/grpc_risk_service.cpp), `app/risk_uc.cpp` | F-07 |
| `SEQ-RISK-NNN-grouped-pretrade-check` (TBD) | [cpp/risk/src/domain/grouped_risk_check.hpp](../../cpp/risk/src/domain/grouped_risk_check.hpp) | F-09 (T-F09-040) |

### Ledger service (`cpp/ledger/`)

| L2 sequence (TODO) | Implementation | Reference |
| --- | --- | --- |
| `SEQ-LEDGER-NNN-apply-fill` (TBD) | `cpp/ledger/src/app/ledger_uc.cpp` | F-06 |

### Backtest service (`cpp/backtest/`)

| L2 sequence (TODO) | Implementation | Reference |
| --- | --- | --- |
| `SEQ-BACKTEST-NNN-replay-session` (TBD) | [cpp/backtest/src/app/run_replay_session_uc.cpp](../../cpp/backtest/src/app/run_replay_session_uc.cpp) | F-15, ADR-009 |
| `SEQ-BACKTEST-NNN-shadow-ledger` (TBD) | [cpp/backtest/src/infra/in_memory_shadow_ledger.cpp](../../cpp/backtest/src/infra/in_memory_shadow_ledger.cpp) | ADR-015 isolation |

### Gateway (`cpp/gateway/`)

| L2 sequence (TODO) | Implementation | Reference |
| --- | --- | --- |
| `SEQ-GATEWAY-NNN-rest-routes` (TBD) | [cpp/gateway/src/transport/http_gateway.cpp](../../cpp/gateway/src/transport/http_gateway.cpp) | F-02 (entry) |

## How this matrix grows

Когда создаётся новая L2 sequence:

1. Создать `docs/05-components/{component}/sequences/SEQ-{COMP}-NNN-*.md` с frontmatter:
   - `level: fish`
   - `component: {component-name}`
   - `expands-step: ../../../05-components/sequences/SEQ-{F}-{UC}-services.md#step-N`
   - `implemented-by:` список `cpp/{component}/src/...` файлов.
   - `tested-by:` список `cpp/{component}/tests/...`.
2. Заменить строку `(TBD)` в этой матрице на реальный link.
3. Дополнить parent L1 sequence (`SEQ-{F}-{UC}-services.md`) таблицей
   step-N ↔ L2 sequence.

## Open questions

| OQ | Описание |
| --- | --- |
| OQ-IN013-04 | Автогенератор этой матрицы: распарсить frontmatter всех `SEQ-*.md` с `level: fish`, собрать `implemented-by` ссылки → таблица. |
| OQ-IN013-05 | L2 sequences для венчурных сервисов (risk, ledger) — backlog. |

## Связанные матрицы

| Матрица | Что показывает |
| --- | --- |
| [feature-to-uc.md](feature-to-uc.md) | F-XX → UCs |
| [uc-to-sequences.md](uc-to-sequences.md) | UC → L0/L1/L2 sequences |
| [sequence-to-code.md](sequence-to-code.md) (этот) | L2 → cpp/ files |
| [coverage-matrix.md](coverage-matrix.md) | Feature → stage artifacts |
