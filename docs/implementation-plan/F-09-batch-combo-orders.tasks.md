# Implementation Tasks: F-09 Batch, Combo and Multi-leg Orders

## Progress

**MVP-1 (`orchestration_only`) + MVP-2 grouped vector solver — реализованы и проверены (PR #13).**
MVP-1: order_flow combo create/cancel, e2e + unit 5/5 зелёные. MVP-2: grouped
solver (strict/scalable/best_effort) собран в `matching_loop` и работает **live
end-to-end** (gRPC CreateComboOrder multileg → matching load→solve→ExecutionGroup
→ Kafka `execution.groups` + PG `execution_groups`), single-leg F-04 не затронут.
Осталось: ledger grouped postings (060), risk group (040), полный E2E до ledger
(090) и фидбэк `filled_cum` для partial-групп (см. Known Gaps в feature README).

| Артефакт | Статус |
| --- | --- |
| feature.yaml | ✅ |
| UC-F09-01/02/03 | ✅ |
| System-level sequences (02-system/use-cases/*/sequences/) | ✅ |
| Service-level sequences SEQ-F09-01/02/03 | ✅ |
| ADR-031, ADR-032, ADR-033 | ✅ |
| gRPC contract (order-flow-create-combo-order.md) | ✅ sketch, TODO proto file |
| Kafka contract (execution-groups.md) | ✅ sketch, TODO proto file |
| OLTP schema (oltp-schema.md F-09 section) | ✅ description, TODO DDL in init.sql |
| OLAP schema (olap-schema.md F-09 section) | ✅ description, TODO DDL in CH init |
| acceptance-criteria.md | ✅ |
| Test plan | ✅ docs/10-testing/features/F-09-test-plan.md |
| Proto files combo.proto / execution_group.proto / order_flow_service ext | ✅ T-F09-010/011/012 (protoc green, backward-compat) |
| Kafka topic execution.groups + backtest.execution.groups | ✅ T-F09-020/023 (bash -n green) |
| DDL tables infra/postgres/init.sql (7) + clickhouse/init.sql (5) | ✅ T-F09-021/022 |
| Application code (order_flow MVP-1) | ✅ T-F09-030/031/033/034/035/036 — order_flow собирается+линкуется на dev-хосте (exit 0). Runtime combo gated on `ORDER_FLOW_POSTGRES_DSN`. Unit-тесты компилируются под `BUILD_TESTING` (не в docker-деплое). |
| Application code (matching grouped, MVP-2) | ✅ T-F09-043/044/045/046/047/048 + `Decimal::div` — feasible caps, grouped solver, child graph, SolveGroupedBatch, ExecutionGroups producer(Kafka)/repo(PG), active groups loader, **интеграция в `matching_loop`**. 9 unit/integration сьютов green (3 против live PG) + **live end-to-end** (execution_groups: filled, scale=1.0). |
| Application code (flags / proto enrich / ledger / E2E) | ✅ T-F09-002 (ComboPolicy флаги+лимиты, create-gate), T-F09-062 (ExecutionGroup += user_id + LegResult symbol/side; combo_orders += user_id/account_id), T-F09-060 (ledger ApplyExecutionGroup, идемпотентно). **GAP-1 `filled_cum` — закрыт.** Полный путь **combo → matching → execution.groups → ledger position проверен LIVE** (T-F09-090). |
| Application code (risk grouped) | ✅ T-F09-040 — `GroupPreTradeCheck` domain (AC-F09-006 + notional/legs/external лимиты) + `PreTradeCheckGroup` gRPC + order_flow `RiskClient` wiring (заменил stub-approve, fail-closed). Verified live (accept + fail-closed proof) + 5 unit-кейсов. |
| **MVP-2 итог** | ✅ **ЗАВЕРШЁН.** Полная вертикаль grouped combo: order_flow (policy+risk gate) → matching (grouped solver) → execution.groups (Kafka+PG) → ledger positions. Остаётся MVP-3+ (spread/factor QP, OCO/bracket, external legs, frontend/observability). |

---

## Корректировки плана по System-Impact Analysis (F-09 v2, 2026-06-09)

Прислан системный impact-анализ F-09. Он **подтверждает** фазовую стратегию
(MVP-1…5 ≈ Phases 0–7 документа): orchestration_only → scalable_atomic
pair/basket → spread/OCO/bracket → external → replay/observability. Структуру
MVP менять не нужно. Формула scale документа `α_g = min_ℓ(Q_feasible/Q_target)`
совпадает с реализованным `GroupedSolverBisection`. Ниже — дополнения (gaps),
которых не хватало явно.

### Добавления к плану

1. **T-F09-002 (Phase 0, НОВАЯ, высокий приоритет): feature flags + grouped policy.**
   Документ §6 ставит это ПЕРВЫМ шагом. Сейчас multileg gated неявно на
   `MATCHING_POSTGRES_DSN`. Нужно явно: флаги `f09_grouped_orders_enabled`,
   `f09_orchestration_only_enabled`, `f09_multileg_vector_solver_enabled`,
   `f09_external_compensating_enabled` + policy `maxLegsPerGroup`,
   `maxChildrenPerGraph`, `allowedComboTypes`, `allowedAtomicityPolicies`,
   `ratioToleranceBps`, `maxWeightDeviationBps`, `requirePreviewBeforeSubmit`,
   `maxGroupedSolveTimeMs`. order_flow/risk читают policy.

2. **AC-F09-011 (НОВАЯ, hard AC): честные гарантии режима.** Главный риск
   (документ §9) — выдать `orchestration_only` за настоящий multi-leg. Каждый
   grouped order в API-ответе и UI ОБЯЗАН показывать `executionMode` + фактические
   гарантии: orchestration_only → нет гарантии ratio/weights/spread;
   multileg_vector_solver → гарантии в пределах policy/tolerance;
   external_compensating → не атомарно. (Сейчас есть только WARN-лог — поднять до
   контракта API/UI.)

3. **T-F09-062 (НОВАЯ, prereq для T-F09-060): обогащение `ExecutionGroup` proto.**
   Для ledger-постинга в позиции и цепочки трассировки нужны: `user_id` на уровне
   группы, `instrument_symbol` + `side` в `LegResult`, связь `fill_id →
   ledgerPostingId`. Producer (`BuildExecutionGroup`) уже имеет эти данные.
   Аддитивно, backward-compat (ADR-033). **Делать ДО T-F09-060.**

4. **Traceability AC:** сквозная цепочка `parentOrderId → executionGroupId →
   legFillId → ledgerPostingId` (документ §3.2) — обеспечить в 046/047/060.

5. **Расширенный scope (MVP-4+), зафиксировать как ориентир:**
   - Frontend grouped — не 2 задачи (080/081), а surface по вкладкам
     trade/profile/pnl/live/alerts/override/policy/replay (документ §3).
   - Alert taxonomy (T-F09-070): `orphan_leg_prevented`, `strict_atomic_rejected`,
     `scalable_scaled_down`, `best_effort_degraded`, `external_compensation_required`,
     `spread_constraint_failed`, `stale_reference_price`, `duplicate_group_event`,
     `grouped_ledger_apply_failed` (документ §3.7).
   - Operator grouped override (F-16): force cancel parent/branch, approve
     degradation, trigger compensation, retry (идемпотентно), freeze group
     (документ §3.8). Правило: override НЕ превращает partial external в strict atomic.
   - Hierarchical PnL: leg/group/parent/strategy + ratio_deviation_cost +
     compensation_pnl + margin_impact (документ §3.5).

### Уточнённый порядок остатков MVP-2

1. **T-F09-002** feature flags + policy (быстро; разблокирует честное gating).
2. **T-F09-062** ExecutionGroup enrichment (prereq для ledger).
3. **T-F09-060** ledger grouped postings + traceability chain.
4. **T-F09-040** risk `PreTradeCheckGroup` (читает policy из T-F09-002).
5. **T-F09-090** E2E basket до ledger postings.

> Документ как источник можно формально заингестить как IN-012 (сейчас не в
> `incoming-docs/` — хук не сработал на mojibake). Для полной traceability —
> по отдельному запросу.

## Source Artifacts

| Тип | Путь |
| --- | --- |
| Feature | `docs/02-system/features/F-09-batch-combo-orders/feature.yaml` |
| Acceptance criteria | `docs/02-system/features/F-09-batch-combo-orders/acceptance-criteria.md` |
| UC-F09-01 | `docs/02-system/use-cases/UC-F09-01-create-combo-order/use-case.md` |
| UC-F09-02 | `docs/02-system/use-cases/UC-F09-02-grouped-matching/use-case.md` |
| UC-F09-03 | `docs/02-system/use-cases/UC-F09-03-external-leg-execution/use-case.md` |
| Service sequence 01 | `docs/05-components/sequences/SEQ-F09-UC-F09-01-services.md` |
| Service sequence 02 | `docs/05-components/sequences/SEQ-F09-UC-F09-02-services.md` |
| Service sequence 03 | `docs/05-components/sequences/SEQ-F09-UC-F09-03-services.md` |
| gRPC contract | `docs/06-api/grpc/order-flow-create-combo-order.md` |
| Kafka contract | `docs/06-api/messaging/execution-groups.md` |
| OLTP schema | `docs/07-data/oltp-schema.md` (F-09 section) |
| OLAP schema | `docs/07-data/olap-schema.md` (F-09 section) |
| ADR-031 | `docs/03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md` |
| ADR-032 | `docs/03-architecture/adr/ADR-032-parent-child-order-model.md` |
| ADR-033 | `docs/03-architecture/adr/ADR-033-execution-groups-topic.md` |
| Incoming source | `incoming-docs/2026-06-05-F-09-batch-combo-orders-v2.md` (IN-011) |

---

## Preconditions Checklist

- [x] Feature document exists: `docs/02-system/features/F-09-batch-combo-orders/feature.yaml`
- [x] Use cases exist: UC-F09-01, UC-F09-02, UC-F09-03
- [x] System-level sequences exist (in `docs/02-system/use-cases/*/sequences/`)
- [x] Service-level sequences exist: SEQ-F09-UC-F09-01/02/03
- [x] Contracts described: gRPC combo.proto sketch in `order-flow-create-combo-order.md`; execution_group.proto sketch in `execution-groups.md`
- [x] Data objects described: 7 OLTP tables + 5 OLAP tables in schema docs
- [x] Acceptance criteria exist: `acceptance-criteria.md` (AC-F09-001..010)
- [x] ADRs accepted: ADR-031, ADR-032, ADR-033
- [x] Test plan: `docs/10-testing/features/F-09-test-plan.md` (T-F09-001 ✅)
- [x] Proto files materialized: combo.proto, execution_group.proto, order_flow_service +5 rpc (T-F09-010/011/012 ✅, protoc green)
- [x] DDL in `infra/postgres/init.sql` (7 tables) + `infra/clickhouse/init.sql` (5 tables) — T-F09-021/022 ✅
- [x] Kafka topics `execution.groups` + `backtest.execution.groups` in `create_topics.sh` — T-F09-020/023 ✅

> Implementation tasks may proceed once all items are checked.
> Test plan (T-F09-001) must precede all code tasks.

---

## Phase Overview and Dependency Graph

```
Phase A: Test Plan (T-F09-001)
    ↓
Phase B: Contracts (T-F09-010..013)
    ↓
Phase C: Data / Infra (T-F09-020..025) — parallel after B
    ↓
Phase D: order_flow domain + app + transport (T-F09-030..036) — after B + C
    ↓
Phase E: risk grouped (T-F09-040..042) — after D
Phase F: matching grouped solver (T-F09-043..049) — after D + C
    ↓ (both)
Phase G: venues / execution-planning external (T-F09-050..052) — after F
Phase H: ledger grouped postings (T-F09-060..062) — after F
Phase I: observability grouped (T-F09-070..071) — after F
Phase J: frontend (T-F09-080..082) — after D
Phase K: integration + E2E tests (T-F09-090..096) — after all service phases
```

---

## Phase A: Test Plan

### T-F09-001: Создать F-09 Test Plan

**PR name:** `PR-F09-001 — docs(F-09): add test plan F-09-test-plan.md`
**Goal:** Создать полный план тестирования F-09 перед написанием любого кода.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001..010
**Estimated diff:** ≤ 120 lines

**Target files:**
- `docs/10-testing/features/F-09-test-plan.md` — создать; перечислить 11 unit-тестов, 11 integration-тестов, 5 negative-тестов из IN-011 §18
- `docs/02-system/features/F-09-batch-combo-orders/feature.yaml` — обновить поле `tests.plan`

**Non-target files:**
- `contracts/proto/` — не трогать
- `cpp/` — не трогать

**Linked DTOs / proto:** нет

**Tests to add:** (документ сам является test plan)

**Acceptance criteria:**
- Файл `docs/10-testing/features/F-09-test-plan.md` содержит все тест-кейсы из IN-011 §18.1..18.3
- feature.yaml поле `tests.plan` указывает на этот файл
- Каждый тест-кейс трассирован к AC-F09-NNN

**Definition of Done:**
- Build: не требует сборки
- Tests: n/a
- Documentation: feature.yaml обновлён, preconditions checklist этого файла обновлён
- Commit message: `docs(F-09): add test plan F-09-test-plan.md`

**Rollback:** `git revert <sha>` безопасен (только docs)

**Risks:** При отсутствии теста в acceptance-criteria.md появляются пробелы в трассируемости.

---

## Phase B: Contracts (Proto)

### T-F09-010: Создать `contracts/proto/fob/orders/v1/combo.proto`

**PR name:** `PR-F09-010 — feat(F-09, proto): add combo.proto — BatchOrder, ComboOrder, Leg, enums`
**Goal:** Материализовать авторизованный proto-sketch из `order-flow-create-combo-order.md` в реальный `.proto` файл.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-003, AC-F09-004, AC-F09-005
**Estimated diff:** ≤ 180 lines

**Target files:**
- `contracts/proto/fob/orders/v1/combo.proto` — создать: enums (`ExecutionMode`, `AtomicityPolicy`, `AtomicityScope`, `ComboType`, `RatioBasis`, `ConstraintSeverity`, `ConditionalLinkType`, `LegStatus`, `ParentOrderStatus`); messages (`Leg`, `MultiLegConstraint`, `ConditionalLink`, `BatchOrder`, `ChildRef`, `ComboOrder`); Request/Response messages для всех 5 методов
- `contracts/CMakeLists.txt` — добавить `combo.proto` в список файлов

**Non-target files:**
- `contracts/proto/fob/orders/v1/orders.proto` — не менять существующие теги
- `contracts/proto/fob/orders/v1/order_flow_service.proto` — не трогать в этой задаче (T-F09-012)
- `cpp/` — не трогать

**Linked DTOs / proto:**
- `contracts/proto/fob/orders/v1/combo.proto` (новый)
- `contracts/proto/fob/common/v1/common.proto` (импортировать, не менять)

**Tests to add:**
- `contracts/tests/combo_proto_build_test.sh` — убедиться что `protoc` компилирует без ошибок
- В `contracts/CMakeLists.txt` убедиться что новый файл включён в target `contracts_proto`

**Acceptance criteria:**
- `cmake --build build --target contracts_proto` зелёный
- Все теги в `Leg` совпадают со sketch в `order-flow-create-combo-order.md` (теги 1..13)
- Enum `ExecutionMode` содержит ровно 3 значения: UNSPECIFIED/ORCHESTRATION_ONLY/MULTILEG_VECTOR_SOLVER

**Definition of Done:**
- Build: `cmake --build build --target contracts_proto` зелёный
- Tests: proto компилируется protoc без warnings
- Documentation: `docs/06-api/grpc/order-flow-create-combo-order.md` — обновить статус с "planned" на "materialized"
- Commit message: соответствует `PR-F09-010`

**Rollback:** `git revert <sha>` безопасен (нет миграций, нет breaking changes к существующим proto)

**Risks:** Теги полей должны совпасть со sketch — любое изменение после релиза потребует ADR. Проверить отсутствие конфликтов имён пакетов с `fob.orders.v1`.

---

### T-F09-011: Создать `contracts/proto/fob/matching/v1/execution_group.proto`

**PR name:** `PR-F09-011 — feat(F-09, proto): add execution_group.proto — ExecutionGroup, LegResult, GroupSolverDiagnostics`
**Goal:** Материализовать proto-схему топика `execution.groups` из `execution-groups.md`.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-003, AC-F09-004, AC-F09-005, AC-F09-009, AC-F09-010
**Estimated diff:** ≤ 120 lines

**Target files:**
- `contracts/proto/fob/matching/v1/execution_group.proto` — создать: enum `GroupStatus` (9 значений); messages `GroupSolverDiagnostics`, `LegResult`, `ExecutionGroup`
- `contracts/CMakeLists.txt` — добавить в список

**Non-target files:**
- `contracts/proto/fob/matching/v1/batch.proto` — не менять существующую `FlowFill` в этой задаче

**Linked DTOs / proto:**
- `contracts/proto/fob/matching/v1/execution_group.proto` (новый)
- `contracts/proto/fob/orders/v1/combo.proto` (импорт — зависит от T-F09-010)
- `contracts/proto/fob/common/v1/common.proto` (импорт)

**Tests to add:**
- proto компилируется без ошибок; `ExecutionGroup.execution_group_id` существует; `LegResult.exec_price` имеет тип `fob.common.v1.Decimal` (не double)

**Acceptance criteria:**
- `GroupSolverDiagnostics.group_residual_norm` имеет тип `double` (допустимо по CLAUDE.md §9)
- Все финансовые поля (`exec_qty`, `exec_price`, `exec_notional`, `execution_scale`) имеют тип `fob.common.v1.Decimal`
- `ExecutionGroup.meta` поле 1 типа `fob.common.v1.EventMeta`

**Definition of Done:**
- Build: `cmake --build build --target contracts_proto` зелёный
- Зависит от: T-F09-010 (combo.proto должен существовать для импорта)

**Rollback:** `git revert <sha>` безопасен

**Risks:** Если batch.proto FlowFill нужно расширить тегами 20+ (для LegFill) — это отдельный breaking change, вынести в отдельную задачу (T-F09-013).

---

### T-F09-012: Расширить `order_flow_service.proto` методами CreateComboOrder/CreateBatchOrder/Preview/Cancel/Get

**PR name:** `PR-F09-012 — feat(F-09, proto): extend OrderFlowService with 5 combo rpc methods`
**Goal:** Добавить 5 новых rpc в `OrderFlowService` без изменения существующих методов.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001
**Estimated diff:** ≤ 30 lines

**Target files:**
- `contracts/proto/fob/orders/v1/order_flow_service.proto` — добавить `import "fob/orders/v1/combo.proto";`; добавить 5 rpc в `service OrderFlowService` (только новые rpc, теги 1..N существующих не трогать)

**Non-target files:**
- `contracts/proto/fob/orders/v1/orders.proto` — не трогать
- `contracts/proto/fob/orders/v1/combo.proto` — только использовать (создан в T-F09-010)

**Linked DTOs / proto:**
- `contracts/proto/fob/orders/v1/combo.proto` (зависит от T-F09-010)
- `contracts/proto/fob/orders/v1/order_flow_service.proto`

**Tests to add:**
- `contracts/tests/order_flow_service_methods_test.sh` — проверить что proto компилируется и все 5 методов присутствуют

**Acceptance criteria:**
- Существующие `CreateFlowOrder`, `CancelFlowOrder`, `GetFlowOrder` rpc неизменны (backward-compatible)
- Новые 5 rpc: `CreateBatchOrder`, `CreateComboOrder`, `PreviewComboOrder`, `CancelComboOrder`, `GetComboOrder` присутствуют

**Definition of Done:**
- Build: зелёный
- Зависит от: T-F09-010

**Rollback:** `git revert <sha>` безопасен (добавление rpc в proto — backward-compatible)

**Risks:** Неверный импорт или конфликт имён типов может сломать существующий `CreateFlowOrder` компиляцию.

---

### T-F09-013: Расширить `batch.proto` FlowFill тегами 20+ для LegFill-контекста

**PR name:** `PR-F09-013 — feat(F-09, proto): extend FlowFill with combo leg context fields (tags 20+)`
**Goal:** Добавить backward-compatible поля `parent_order_id`, `execution_group_id`, `leg_id`, `group_policy` в `FlowFill` (теги 20+) для combo-ног в топике `fills`.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-009, AC-F09-010
**Estimated diff:** ≤ 30 lines

**Target files:**
- `contracts/proto/fob/matching/v1/batch.proto` — добавить поля 20..23 в `FlowFill`; добавить комментарий `// F-09: combo leg context fields — backward compatible, existing consumers ignore unknown fields`

**Non-target files:**
- `contracts/proto/fob/matching/v1/execution_group.proto` — отдельная задача
- `cpp/matching/` — не трогать в этой задаче

**Linked DTOs / proto:**
- `contracts/proto/fob/matching/v1/batch.proto`

**Tests to add:**
- Сериализация/десериализация `FlowFill` без новых полей всё ещё работает (регрессия) — тест в `cpp/matching/tests/`

**Acceptance criteria:**
- Существующие consumers `batch.outputs` / `fills` без перекомпиляции не ломаются (proto3 unknown fields игнорируются)
- Новые теги строго >= 20 (не конфликтуют с существующими 1..N)
- `group_policy` — тип `string` (не enum, для читаемости, согласно sketch)

**Definition of Done:**
- Build: зелёный
- Регрессионный тест существующего FlowFill зелёный

**Rollback:** `git revert <sha>` безопасен (proto3 backward-compatible addition)

**Risks:** Конфликт тегов если существующие поля достигли 20. Перед коммитом проверить максимальный тег в `FlowFill`.

---

## Phase C: Data / Infra

### T-F09-020: Kafka topic `execution.groups` в `create_topics.sh`

**PR name:** `PR-F09-020 — devops(F-09): register execution.groups Kafka topic`
**Goal:** Добавить топик `execution.groups` в `infra/kafka/create_topics.sh` и `docs/06-api/messaging/topics.md`.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-009, AC-F09-010
**Estimated diff:** ≤ 20 lines

**Target files:**
- `infra/kafka/create_topics.sh` — добавить секцию F-09: `rpk topic create execution.groups --partitions 12 --replicas 1 --topic-config retention.ms=604800000`; комментарий `# F-09: grouped execution results (ADR-033, key=parentOrderId)`
- `docs/06-api/messaging/topics.md` — добавить строку топика `execution.groups` в таблицу

**Non-target files:**
- `contracts/proto/` — не трогать
- `cpp/` — не трогать

**Linked DTOs / proto:**
- Топик `execution.groups`, schema `fob.matching.v1.ExecutionGroup`

**Tests to add:**
- `Testing/f09_topics_smoke.sh` — создать: проверить что `rpk topic describe execution.groups` возвращает retention 7d и нужное количество partitions

**Acceptance criteria:**
- `infra/kafka/create_topics.sh` идемпотентен (повторный вызов не падает, согласно существующему паттерну)
- `execution.groups` появляется в `rpk topic list` после `docker compose up`

**Definition of Done:**
- Build: `docker compose -f infra/docker-compose.dev.yml up redpanda` + `bash infra/kafka/create_topics.sh` без ошибок
- Зависит от: T-F09-010, T-F09-011 (proto должен существовать до того как producer будет писать в топик)

**Rollback:** `git revert <sha>` безопасен; топик может остаться в кластере — удалить вручную `rpk topic delete execution.groups`

**Risks:** Количество partitions для production нужно пересмотреть (текущий dev: 12 = согласно существующим топикам).

---

### T-F09-021: DDL PostgreSQL — 7 таблиц F-09 в `infra/postgres/init.sql`

**PR name:** `PR-F09-021 — devops(F-09): add F-09 OLTP tables DDL to init.sql`
**Goal:** Добавить DDL для 7 OLTP таблиц F-09 в `infra/postgres/init.sql`.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-007, AC-F09-008, AC-F09-009
**Estimated diff:** ≤ 200 lines

**Target files:**
- `infra/postgres/init.sql` — добавить блок `-- F-09: Batch/Combo/Multi-leg Orders (ADR-032)` с CREATE TABLE IF NOT EXISTS для: `batch_orders`, `combo_orders`, `combo_order_legs`, `combo_constraints`, `conditional_links`, `execution_groups`, `group_state_transitions`; CREATE INDEX для каждой таблицы согласно олтп-схеме

**Non-target files:**
- `docs/07-data/oltp-schema.md` — не менять (уже описано)
- `cpp/` — не трогать

**Linked DTOs / proto:** DDL из `docs/07-data/oltp-schema.md` F-09 секции

**Tests to add:**
- `Testing/f09_schema_smoke.sh` — запустить `psql` SELECT на каждую из 7 таблиц после `docker compose up postgres` + `init.sql`

**Acceptance criteria:**
- `combo_order_legs` — отдельная таблица (не расширение `flow_order_legs`), согласно рекомендации oltp-schema.md
- `execution_groups.execution_group_id` — UUID PK, idempotency key
- `group_state_transitions.idempotency_key` — UNIQUE constraint
- CHECK `p_low > 0`, `p_high >= p_low` в `combo_order_legs`
- CHECK `filled_cum BETWEEN 0 AND q_max` в `combo_order_legs`
- Все FK ссылки консистентны

**Definition of Done:**
- Build: `docker compose -f infra/docker-compose.dev.yml up postgres` + `psql -f init.sql` без ошибок
- `Testing/f09_schema_smoke.sh` зелёный

**Rollback:** Требует down-миграции: `DROP TABLE IF EXISTS group_state_transitions, execution_groups, conditional_links, combo_constraints, combo_order_legs, combo_orders, batch_orders CASCADE;`

**Risks:** init.sql — idempotent script (используется `IF NOT EXISTS`), безопасен. Порядок CREATE TABLE важен из-за FK: сначала `batch_orders`, затем `combo_orders`, `combo_order_legs`, `combo_constraints`, `conditional_links`, затем `execution_groups`, `group_state_transitions`.

---

### T-F09-022: DDL ClickHouse — 5 аналитических таблиц F-09

**PR name:** `PR-F09-022 — devops(F-09): add F-09 OLAP tables DDL for ClickHouse`
**Goal:** Добавить DDL для 5 ClickHouse таблиц F-09 в clickhouse init или отдельный файл.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-005, AC-F09-009, AC-F09-010
**Estimated diff:** ≤ 150 lines

**Target files:**
- `infra/clickhouse/init-f09.sql` (или дополнение к существующему clickhouse init) — CREATE TABLE IF NOT EXISTS: `grouped_execution_events`, `grouped_leg_fills`, `grouped_quality_metrics`, `grouped_ratio_deviation`, `grouped_replay_results`; MaterializedView для `grouped_quality_metrics` из `grouped_execution_events + grouped_leg_fills`
- `docs/07-data/olap-schema.md` — обновить статус F-09 секции на "DDL materialized"

**Non-target files:**
- `infra/postgres/init.sql` — не трогать

**Linked DTOs / proto:** OLAP DDL из `docs/07-data/olap-schema.md` F-09 секции

**Tests to add:**
- `Testing/f09_olap_smoke.sh` — SELECT count(*) FROM каждой таблицы после `docker compose up clickhouse`

**Acceptance criteria:**
- `grouped_execution_events` партиционирована по `toYYYYMMDD(created_at)`
- `grouped_replay_results` изолирована от live топика (инgestится из `backtest.execution.groups`, не из `execution.groups`)
- MV `grouped_quality_metrics` корректно считает combined VWAP

**Definition of Done:**
- Build: docker compose up clickhouse + init-f09.sql без ошибок
- Зависит от: T-F09-011 (знать schema ExecutionGroup)

**Rollback:** `DROP TABLE IF EXISTS ...` в обратном порядке; безопасен (OLAP-only, нет финансового состояния)

**Risks:** ClickHouse может не быть в docker-compose.dev.yml — проверить наличие и добавить если нужно.

---

### T-F09-023: Kafka topic `backtest.execution.groups` в `create_topics.sh`

**PR name:** `PR-F09-023 — devops(F-09): register backtest.execution.groups isolated replay topic`
**Goal:** Зарегистрировать изолированный топик для replay grouped execution (AC-F09-010), аналогично `backtest.execution.venue`.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-010
**Estimated diff:** ≤ 15 lines

**Target files:**
- `infra/kafka/create_topics.sh` — добавить `backtest.execution.groups`
- `docs/06-api/messaging/topics.md` — добавить строку

**Non-target files:** все остальные

**Tests to add:** дополнить `Testing/f09_topics_smoke.sh` проверкой `backtest.execution.groups`

**Acceptance criteria:**
- Топик существует и изолирован от `execution.groups` (разные consumer groups для replay)

**Definition of Done:** как T-F09-020

**Rollback:** `git revert <sha>` безопасен

**Risks:** Если backtest-сервис ещё не читает `backtest.execution.groups` — нормально, задача только регистрирует топик.

---

## Phase D: order_flow — Domain, App, Transport

### T-F09-030: Domain: ComboOrder, Leg, MultiLegConstraint, ConditionalLink + инварианты

**PR name:** `PR-F09-030 — feat(F-09, order-flow): domain entities ComboOrder/Leg/Constraint/Link + invariants`
**Goal:** Добавить domain-слой для parent/child модели: сущности, value objects, инварианты.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-002, AC-F09-003, AC-F09-004
**Estimated diff:** ≤ 180 lines

**Target files:**
- `cpp/order_flow/src/domain/combo_order.hpp` — `ComboOrder`, `BatchOrder`, `Leg`, `MultiLegConstraint`, `ConditionalLink`; статусы (enum `ParentOrderStatus`, `LegStatus`); инварианты: `p_low > 0`, `p_high >= p_low`, `q_rate > 0`, `q_max > 0`, `filled_cum <= q_max`; метод `validate() -> std::expected<void, DomainError>`
- `cpp/order_flow/src/domain/combo_order.cpp` — реализация `validate()` и инвариантов
- `cpp/order_flow/tests/combo_order_domain_test.cpp` — тест `NormalizeComboOrder`, проверка каждого инварианта

**Non-target files:**
- `cpp/order_flow/src/domain/flow_order.hpp` — не трогать
- `cpp/matching/` — не трогать

**Linked DTOs / proto:**
- `contracts/proto/fob/orders/v1/combo.proto` (зависит от T-F09-010)

**Tests to add:**
- Unit test `NormalizeComboOrder` — позитивный кейс basket с 2 ногами
- Unit test `ComboOrderInvariantViolation` — p_low > p_high → ошибка
- Unit test `LegFilledCumExceedsQMax` → ошибка

**Acceptance criteria:**
- `combo_order.validate()` отклоняет заявку с нарушенными инвариантами
- При `executionMode = ORCHESTRATION_ONLY` и отсутствии constraints — валидация проходит
- При `executionMode = MULTILEG_VECTOR_SOLVER` без `atomicityPolicy` → ошибка (обязательно)

**Definition of Done:**
- Build: `cmake --build build -j` зелёный
- Tests: `ctest -R combo_order_domain_test` passing
- Зависит от: T-F09-010 (proto), T-F09-021 (DDL не требуется для domain)

**Rollback:** `git revert <sha>` безопасен (только новые файлы)

**Risks:** Надо решить использует ли `ComboOrder` proto-типы напрямую или собственные value objects. Рекомендуется собственные value objects в domain — proto только на transport-слое.

---

### T-F09-031: App UseCase: CreateComboOrderUseCase + idempotency

**PR name:** `PR-F09-031 — feat(F-09, order-flow): app CreateComboOrderUseCase with idempotency`
**Goal:** Application use case принятия и нормализации ComboOrder: валидация, risk check (mock), persist, publish.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-006
**Estimated diff:** ≤ 180 lines

**Target files:**
- `cpp/order_flow/src/app/create_combo_order_use_case.hpp` — интерфейс + порты: `IComboOrderRepository`, `IRiskService`, `IMarketDataClient`, `IOrderNormalizedProducer`
- `cpp/order_flow/src/app/create_combo_order_use_case.cpp` — реализация: normalize parent + legs + constraints + graph links; call `IRiskService::PreTradeCheckGroup`; idempotency по `client_combo_id`; save; publish `orders.normalized`; emit `orchestration_only` warning
- `cpp/order_flow/tests/create_combo_order_use_case_test.cpp` — тест `ApiToComboOrderPersistAndPublish`, `DuplicateClientComboIdIdempotent`

**Non-target files:**
- `cpp/order_flow/src/domain/flow_order.hpp` — не трогать
- `cpp/matching/` — не трогать

**Linked DTOs / proto:**
- `contracts/proto/fob/orders/v1/combo.proto`, `order_flow_service.proto` (зависит от T-F09-010, T-F09-012)

**Tests to add:**
- `CreateComboOrderUseCase` с mock RiskService (approved): parent сохранён, опубликован в `orders.normalized`
- Idempotency: второй вызов с тем же `client_combo_id` возвращает существующий `combo_id` без INSERT
- `orchestration_only` + constraints → warning emitted

**Acceptance criteria:**
- Use case отклоняет `executionMode = MULTILEG_VECTOR_SOLVER` с `atomicityScope = EXTERNAL_COMPENSATING` и `atomicityPolicy = STRICT_ATOMIC` (нарушение ADR-031)
- Idempotency по `client_combo_id` работает

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R create_combo_order_use_case_test` passing
- Зависит от: T-F09-030 (domain), T-F09-010 (proto)

**Rollback:** `git revert <sha>` безопасен

**Risks:** `IRiskService::PreTradeCheckGroup` — stub или реальный? Реальный — в фазе E. В этой задаче достаточно interface + mock.

---

### T-F09-032: App UseCase: PreviewComboOrderUseCase

**PR name:** `PR-F09-032 — feat(F-09, order-flow): app PreviewComboOrderUseCase grouped preview`
**Goal:** Grouped preview без создания заявки: ожидаемый execution scale, leg fills, combined VWAP, binding leg.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-002, AC-F09-004
**Estimated diff:** ≤ 120 lines

**Target files:**
- `cpp/order_flow/src/app/preview_combo_order_use_case.hpp` — интерфейс + порт `IExecutionPlanningClient`
- `cpp/order_flow/src/app/preview_combo_order_use_case.cpp` — вычисление grouped preview по reference prices; расчёт `feasible caps` для каждой ноги; вычисление expected execution scale
- `cpp/order_flow/tests/preview_combo_order_use_case_test.cpp` — `CalculateRatioDeviation`, `CalculateCombinedVWAP`, `CalculateCombinedIS`

**Non-target files:** `cpp/matching/` — не трогать

**Tests to add:**
- Basket с 2 ногами: binding leg = та у которой меньший feasible cap
- Combined VWAP = sum(execQty * execPrice) / sum(execQty) по формуле CLAUDE.md §8.5

**Acceptance criteria:**
- Preview не создаёт записей в БД (read-only по смыслу)
- При stale reference price → error (AC-F09 negative test `StaleReferencePriceNoSpreadExecution`)

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R preview_combo_order_use_case_test` passing
- Зависит от: T-F09-030, T-F09-031

**Rollback:** `git revert <sha>` безопасен

**Risks:** IExecutionPlanningClient — TODO contract stub в этой задаче.

---

### T-F09-033: App UseCase: CancelComboOrderUseCase + idempotency OCO siblings

**PR name:** `PR-F09-033 — feat(F-09, order-flow): app CancelComboOrderUseCase idempotent with OCO sibling cancel`
**Goal:** Отменить ComboOrder и все активные ноги; идемпотентно отменить OCO-сиблинги.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-007
**Estimated diff:** ≤ 100 lines

**Target files:**
- `cpp/order_flow/src/app/cancel_combo_order_use_case.hpp` + `.cpp`
- `cpp/order_flow/tests/cancel_combo_order_use_case_test.cpp` — `ApplyOCOTransitions`, `DuplicateGroupEventNoDuplicateFills` (cancel idempotency)

**Tests to add:**
- Повторный вызов cancel на уже отменённой заявке → success без side-effects
- OCO: cancel одной ветви → sibling помечается `cancelled` idempotently

**Acceptance criteria:**
- `group_state_transitions` логирует каждый переход с `idempotency_key`
- Повторный cancel не создаёт дублей в `group_state_transitions`

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R cancel_combo_order_use_case_test` passing
- Зависит от: T-F09-030, T-F09-031

**Rollback:** `git revert <sha>` безопасен

**Risks:** OCO-логика требует чтения `conditional_links` — зависит от DDL (T-F09-021).

---

### T-F09-034: Infra: ComboOrderRepository (PostgreSQL)

**PR name:** `PR-F09-034 — feat(F-09, order-flow): infra PostgresComboOrderRepository CRUD`
**Goal:** Репозиторий для 5 OLTP таблиц F-09 со стороны order_flow: INSERT parent/legs/constraints/links, SELECT active, UPDATE status.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-007
**Estimated diff:** ≤ 200 lines

**Target files:**
- `cpp/order_flow/src/infra/postgres_combo_order_repository.hpp` — интерфейс `IComboOrderRepository`
- `cpp/order_flow/src/infra/postgres_combo_order_repository.cpp` — реализация: INSERT `batch_orders` / `combo_orders` / `combo_order_legs` / `combo_constraints` / `conditional_links` (все в одной транзакции); SELECT active by status; UPDATE status; idempotency check `client_combo_id UNIQUE`
- `cpp/order_flow/tests/postgres_combo_order_repository_test.cpp` — интеграционный тест с реальным PG

**Non-target files:**
- `cpp/order_flow/src/infra/postgres_flow_order_repository.{hpp,cpp}` — не трогать

**Linked DTOs / proto:** DDL из T-F09-021

**Tests to add:**
- Insert + select round-trip для basket с 2 ногами
- Idempotency: повторный INSERT с `client_combo_id` → возвращает существующий

**Acceptance criteria:**
- INSERT parent + legs + constraints + links выполняется в одной PG-транзакции (атомарно)
- `NUMERIC(38,18)` для всех Decimal полей (CLAUDE.md §9)

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R postgres_combo_order_repository_test` passing (требует docker PG)
- Зависит от: T-F09-021 (DDL), T-F09-030 (domain types)

**Rollback:** `git revert <sha>` — DDL остаётся (вернуть отдельной down-миграцией)

**Risks:** PG connection в тестах — проверить `TEST_PG_DSN` env var паттерн существующих тестов.

---

### T-F09-035: Infra: OrdersNormalizedGroupedProducer (Kafka publish grouped orders.normalized)

**PR name:** `PR-F09-035 — feat(F-09, order-flow): Kafka producer for grouped orders.normalized`
**Goal:** Публиковать в `orders.normalized` grouped payload (parent + legs + constraints + graph + executionMode + atomicityPolicy) с ключом `parentOrderId`.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001
**Estimated diff:** ≤ 100 lines

**Target files:**
- `cpp/order_flow/src/infra/orders_normalized_grouped_producer.hpp` + `.cpp` — расширение/адаптер существующего producer для grouped envelope
- `cpp/order_flow/tests/orders_normalized_grouped_producer_test.cpp` — mock Kafka produce, verify key = parentOrderId

**Non-target files:**
- Существующий `orders.normalized` producer для `FlowOrder` — не ломать

**Linked DTOs / proto:**
- `contracts/proto/fob/orders/v1/combo.proto` (зависит от T-F09-010)
- `contracts/proto/fob/orders/v1/order_flow_service.proto`

**Tests to add:**
- Проверить что key = `parentOrderId.bytes()` в Kafka record
- Проверить что `executionMode` и `atomicityPolicy` присутствуют в сообщении

**Acceptance criteria:**
- Grouped envelope содержит parent, legs[], constraints[], graphLinks[], executionMode, atomicityPolicy
- Partition key = parentOrderId (строковый UUID → bytes)

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R orders_normalized_grouped_producer_test` passing

**Rollback:** `git revert <sha>` безопасен

**Risks:** Формат envelope `orders.normalized` для grouped должен быть backward-compatible с существующим FlowOrder consumer в matching — нужно согласовать discriminator поле (type = "combo" vs "flow").

---

### T-F09-036: Transport: gRPC handlers для 5 методов OrderFlowService

**PR name:** `PR-F09-036 — feat(F-09, order-flow): gRPC transport handlers for 5 combo methods`
**Goal:** Thin gRPC handlers маппящие proto DTO в application commands и вызывающие use cases.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001
**Estimated diff:** ≤ 160 lines

**Target files:**
- `cpp/order_flow/src/transport/combo_order_grpc_handler.hpp` + `.cpp` — реализация 5 rpc: `CreateBatchOrder`, `CreateComboOrder`, `PreviewComboOrder`, `CancelComboOrder`, `GetComboOrder`; mapping proto → app command → proto response; обработка `Status::ALREADY_EXISTS` для idempotency
- `cpp/order_flow/src/transport/order_flow_grpc_server.cpp` — зарегистрировать новые методы

**Non-target files:**
- Существующие gRPC handlers `FlowOrder` — не трогать

**Tests to add:**
- `cpp/order_flow/tests/combo_order_grpc_handler_test.cpp` — mock use case, verify proto mapping для CreateComboOrder
- Verify `orchestration_only` warning присутствует в response metadata

**Acceptance criteria:**
- `Status::OK` для approve
- `Status::UNPROCESSABLE_ENTITY` / `Status::FAILED_PRECONDITION` для risk reject
- Idempotency: второй `CreateComboOrder` с тем же `client_combo_id` → `Status::OK` + существующий ID

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R combo_order_grpc_handler_test` passing
- Зависит от: T-F09-031..035

**Rollback:** `git revert <sha>` безопасен

**Risks:** gRPC server registration — проверить что service descriptor не конфликтует.

---

## Phase E: Risk — Grouped Pre-Trade и Post-Trade

### T-F09-040: Risk Domain: GroupPreTradeCheck + PreTradeCheckGroup gRPC method

**PR name:** `PR-F09-040 — feat(F-09, risk): PreTradeCheckGroup domain + gRPC method stub`
**Goal:** Добавить grouped pre-trade check: per-leg + per-group по notional, margin, max_legs, atomicity scope.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-006
**Estimated diff:** ≤ 180 lines

**Target files:**
- `cpp/risk/src/domain/grouped_risk_check.hpp` + `.cpp` — `GroupPreTradeResult`; проверки: каждая нога проходит индивидуальный `RiskCheck`; total notional ≤ limit; max legs ≤ config; `atomicityScope = EXTERNAL_COMPENSATING + atomicityPolicy = STRICT_ATOMIC` → REJECT; max external legs ≤ config
- `cpp/risk/src/transport/risk_grpc_handler.cpp` — добавить `PreTradeCheckGroup` rpc
- `cpp/risk/tests/grouped_risk_check_test.cpp` — `RejectExternalStrictAtomicWithoutNativeSupport`

**Non-target files:**
- Существующий `PreTradeCheck` single-order — не трогать

**Tests to add:**
- Negative: `STRICT_ATOMIC + EXTERNAL_COMPENSATING` → `RiskDecision::REJECT`
- Negative: total notional > limit → REJECT
- Positive: basket 2 ноги, всё в пределах → ACCEPT

**Acceptance criteria:**
- `PreTradeCheckGroup` отклоняет `strict_atomic` без `venue_native` для внешних ног (AC-F09-006)
- Grouped rejection логируется в `risk.alerts` с `alertType = GROUPED_PRE_TRADE_REJECTED`

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R grouped_risk_check_test` passing
- Зависит от: T-F09-010 (combo.proto), T-F09-012

**Rollback:** `git revert <sha>` безопасен

**Risks:** TODO contract для `RiskService/PreTradeCheckGroup` — нужно создать `docs/06-api/grpc/risk-pre-trade-check-group.md` (можно в этой задаче или предшествующей).

---

### T-F09-041: Risk: PostTradeGroupedCheck consumer от `execution.groups`

**PR name:** `PR-F09-041 — feat(F-09, risk): post-trade grouped risk check consumer execution.groups`
**Goal:** Risk потребляет `execution.groups` и выполняет post-trade check: ratio deviation, margin level, hard constraints.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-002, AC-F09-005
**Estimated diff:** ≤ 120 lines

**Target files:**
- `cpp/risk/src/infra/execution_groups_consumer.hpp` + `.cpp` — Kafka consumer `execution.groups`, idempotency по `execution_group_id`; publish `risk.alerts` при `ratioDeviationBps > limit` или `violated hard constraint`
- `cpp/risk/tests/post_trade_grouped_check_test.cpp` — `BasketTargetWeightsDeviation` интеграция

**Tests to add:**
- basket fill с ratio_deviation_bps = 10 > tolerance 5 → risk.alerts

**Acceptance criteria:**
- Idempotent: повторная доставка того же `execution_group_id` не создаёт дублей alerts
- `risk.alerts` key = `parentOrderId` (ADR-033)

**Definition of Done:**
- Build: зелёный
- Tests: passing
- Зависит от: T-F09-011 (execution_group.proto), T-F09-020 (топик)

**Rollback:** `git revert <sha>` безопасен

---

## Phase F: Matching — Grouped Solver

### T-F09-043: Matching Domain: MultiLegVectorOrder + feasible caps calculation

**PR name:** `PR-F09-043 — feat(F-09, matching): domain MultiLegVectorOrder + feasible caps`
**Goal:** Добавить в matching domain структуру `MultiLegVectorOrder` и чистую функцию расчёта feasible caps.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-004
**Estimated diff:** ≤ 160 lines

**Target files:**
- `cpp/matching/src/domain/multileg_vector_order.hpp` — `MultiLegVectorOrder`: legs[], signedLegVector, targetRatio, constraintMatrix (A_g, G_g), atomicityPolicy, fallbackPolicy; `FeasibleCap` per leg
- `cpp/matching/src/domain/multileg_feasible_caps.hpp` + `.cpp` — чистая функция `ComputeFeasibleCaps(MultiLegVectorOrder, ReferencePrices) -> vector<FeasibleCap>`; формула: `Q_feasible = min(Q_remaining, Q_rate, Q_liq, Q_risk, Q_venue)`
- `cpp/matching/tests/multileg_feasible_caps_test.cpp` — `BuildMultiLegVectorOrder`, детерминированный ввод → детерминированный вывод

**Non-target files:**
- `cpp/matching/src/domain/solver.{hpp,cpp}` — не трогать в этой задаче (T-F09-044)

**Tests to add:**
- Basket 2 ноги, одна нога ограничена Q_rate → binding cap = leg с меньшим cap
- Детерминизм: тот же ввод → тот же вывод (AC-F09-010)

**Acceptance criteria:**
- `FeasibleCap` использует `Decimal` (не double) для финансовых значений
- Функция `ComputeFeasibleCaps` — pure (без IO, без Kafka, без DB)

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R multileg_feasible_caps_test` passing
- Зависит от: T-F09-010 (combo.proto), T-F09-011

**Rollback:** `git revert <sha>` безопасен

**Risks:** Выбор solver алгоритма (open question из feature.yaml `solver-formulation-open`) — в этой задаче только feasible caps, solver формулировка — в T-F09-044.

---

### T-F09-044: Matching Domain: GroupedSolver — scalable_atomic / strict_atomic / best_effort

**PR name:** `PR-F09-044 — feat(F-09, matching): grouped vector solver scalable+strict+best_effort policies`
**Goal:** Реализовать grouped solver: выбор α_g*, вектора e_g, применение atomicity/fallback policy. MVP — per-symbol bisection (без QP), достаточно для ratio/basket/scalable_atomic.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-002, AC-F09-003, AC-F09-004, AC-F09-005
**Estimated diff:** ≤ 200 lines

**Target files:**
- `cpp/matching/src/domain/grouped_solver.hpp` — interface `IGroupedSolver`; struct `GroupedSolveInput` (MultiLegVectorOrder, feasible caps, reference prices, constraints); struct `GroupedSolveResult` (executionScale, legExecs[], violatedConstraints[], fallbackAction, diagnostics)
- `cpp/matching/src/domain/grouped_solver_bisection.hpp` + `.cpp` — MVP-реализация: для ratio/basket — bisect α_g от 0 до min(caps/target); применение `strict_atomic` (α < min → 0); `scalable_atomic` (e_g = α_liq * ρ_g); `best_effort` (record violations)
- `cpp/matching/tests/grouped_solver_test.cpp` — `SolveGroupedBatchStrictAtomic`, `SolveGroupedBatchScalableAtomic`, `ApplyBestEffortPolicy`; replay determinism тест

**Non-target files:**
- `cpp/matching/src/domain/solver.{hpp,cpp}` (single-leg F-04 solver) — не трогать

**Tests to add:**
- `SolveGroupedBatchStrictAtomic`: одна нога недоступна → scale=0, fills=[]
- `SolveGroupedBatchScalableAtomic`: одна нога ограничена → e_g = α_liq * ρ_g, ratio deviation ≤ 5 bps
- `ApplyBestEffortPolicy`: нарушение ограничения → violatedConstraints non-empty, degraded status
- Replay: тот же вход (2 раза) → идентичный `GroupedSolveResult` (AC-F09-010)
- `IndependentSplitCannotBeStrictAtomic` — negative: попытка strict с независимыми ногами

**Acceptance criteria:**
- `strict_atomic`: `orphanLegs = 0` всегда
- `scalable_atomic`: `ratioDeviationBps ≤ maxRatioDeviationBps`
- solver использует только `Decimal` или преобразует к double только для диагностики
- `GroupedSolveResult` детерминирован при одном и том же входе

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R grouped_solver_test` passing
- Зависит от: T-F09-043

**Rollback:** `git revert <sha>` безопасен (алгоритм за интерфейсом — обратим)

**Risks:** Open question: spread constraint (A_g e_g = b_g α_g) требует линейной системы — в MVP достаточно закомментировать spread case как TODO и реализовать в MVP-3. Это должно быть явно задокументировано в коде (`// TODO F-09 MVP-3: spread/factor constraints require QP solver (OSQP/Eigen)`).

---

### T-F09-045: Matching App: SolveGroupedBatch use case + child graph transitions (OCO/bracket)

**PR name:** `PR-F09-045 — feat(F-09, matching): SolveGroupedBatch use case + OCO/bracket transitions`
**Goal:** Application use case для одного batch cycle: загрузка активных групп, grouped solve, child graph transitions (OCO cancel siblings, bracket resize exits), формирование ExecutionGroup.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-003, AC-F09-004, AC-F09-007, AC-F09-008, AC-F09-010
**Estimated diff:** ≤ 200 lines

**Target files:**
- `cpp/matching/src/app/solve_grouped_batch_use_case.hpp` + `.cpp` — оркестрация: 1) load active groups from repo; 2) update child graph (OCO siblings cancel, bracket entry→TP/SL resize); 3) build MultiLegVectorOrder per group; 4) get reference prices; 5) compute feasible caps; 6) run grouped solver; 7) apply atomicity postprocessor; 8) produce ExecutionGroup; 9) produce LegFills; 10) update parent/leg statuses
- `cpp/matching/src/domain/child_graph_transitions.hpp` + `.cpp` — `ApplyOCOTransitions(parentOrder, legFills)` → cancel sibling legs; `ResizeBracketExits(bracketOrder, entryFilledQty)` → Q_tp = Q_sl = Q_entry_filled; **идемпотентны**: проверка `group_state_transitions` перед записью
- `cpp/matching/tests/solve_grouped_batch_use_case_test.cpp` — интеграция с mock repo + mock Kafka

**Tests to add:**
- `PairStrictAtomicBothLegsLiquid`: оба leg liquid → fill с ratio deviation 0
- `PairStrictAtomicOneLegIlliquid`: одна нога блокирована → scale=0, no fills
- `ApplyOCOTransitions`: leg A filled → sibling B cancelled idempotently
- `ResizeBracketExits`: entry filled 60% → TP/SL max_qty = 60% от исходного
- `DuplicateGroupEventNoDuplicateFills`: повторный solve с тем же batch_id → idempotent

**Acceptance criteria:**
- Commit rule: `ExecutionGroup` публикуется раньше или одновременно с LegFills (ADR-033)
- OCO cancel sibling записывается в `group_state_transitions` с idempotency_key
- Bracket resize: `Q_tp = Q_sl = Q_entry^filled`

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R solve_grouped_batch_use_case_test` passing
- Зависит от: T-F09-043, T-F09-044

**Rollback:** `git revert <sha>` безопасен

**Risks:** Интеграция с основным batch cycle (`RunBatchUseCase` F-04) — нужно решить: отдельный timer или shared cycle. Рекомендация: вызывать `SolveGroupedBatch` в рамках того же batch cycle после single-leg solving (не ломать F-04).

---

### T-F09-046: Matching Infra: ExecutionGroupsProducer (Kafka `execution.groups`)

**PR name:** `PR-F09-046 — feat(F-09, matching): ExecutionGroupsProducer Kafka execution.groups`
**Goal:** Kafka producer для публикации `ExecutionGroup` в топик `execution.groups` (key = parentOrderId).
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-009, AC-F09-010
**Estimated diff:** ≤ 100 lines

**Target files:**
- `cpp/matching/src/infra/execution_groups_producer.hpp` + `.cpp`
- `cpp/matching/tests/execution_groups_producer_test.cpp` — mock produce, verify key, verify GroupStatus enum

**Tests to add:**
- `GroupStatus::GROUP_STATUS_CANCELLED_BY_ATOMICITY` → `leg_results` пустой

**Acceptance criteria:**
- Partition key = `parentOrderId` (согласно ADR-033)
- `executionGroupId` уникален per batch cycle per parent (UUID)

**Definition of Done:**
- Build: зелёный
- Tests: passing
- Зависит от: T-F09-011, T-F09-020

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-047: Matching Infra: ExecutionGroupsRepository (PostgreSQL `execution_groups`)

**PR name:** `PR-F09-047 — feat(F-09, matching): PostgresExecutionGroupsRepository INSERT + idempotency`
**Goal:** Repository для INSERT `execution_groups` и `group_state_transitions` с idempotency.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-009
**Estimated diff:** ≤ 120 lines

**Target files:**
- `cpp/matching/src/infra/postgres_execution_groups_repository.hpp` + `.cpp` — INSERT execution_groups (ON CONFLICT execution_group_id DO NOTHING); INSERT group_state_transitions (ON CONFLICT idempotency_key DO NOTHING); UPDATE combo_orders/combo_order_legs статусы

**Tests to add:**
- INSERT + повторный INSERT с тем же execution_group_id → no-op (idempotent)
- UPDATE combo_order status = partial при scale < 1

**Acceptance criteria:**
- `NUMERIC(38,18)` для `execution_scale`, `leg_results` JSONB сохраняет все LegResult поля
- Транзакция: INSERT execution_group + INSERT transitions + UPDATE combo_orders — в одной PG-транзакции

**Definition of Done:**
- Build: зелёный
- Tests: passing (integration с PG)
- Зависит от: T-F09-021 (DDL), T-F09-045

**Rollback:** `git revert <sha>` — DDL остаётся

---

### T-F09-048: Matching: grouped orders.normalized consumer + active groups loader

**PR name:** `PR-F09-048 — feat(F-09, matching): consume grouped orders.normalized + load active groups`
**Goal:** Matching потребляет `orders.normalized` в grouped-формате и загружает активные группы из PostgreSQL.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001
**Estimated diff:** ≤ 100 lines

**Target files:**
- `cpp/matching/src/infra/orders_normalized_grouped_consumer.hpp` + `.cpp` — Kafka consumer с discriminator по type="combo"; десериализация grouped envelope → `ComboOrderBatchInput`
- `cpp/matching/src/infra/active_grouped_orders_loader.hpp` + `.cpp` — SELECT active combo_orders + legs + constraints + conditional_links

**Tests to add:**
- Deserialization round-trip: proto serialize → deserialize → all fields match
- Loader: активные заявки из PG → MultiLegVectorOrder[]

**Acceptance criteria:**
- `orchestration_only` заявки десериализуются корректно но не передаются в grouped solver (идут как независимые FlowOrder)
- Consumer offset commit после успешной обработки (at-least-once)

**Definition of Done:**
- Build: зелёный
- Зависит от: T-F09-035, T-F09-021

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-049: Matching: replay determinism для grouped execution (AC-F09-010)

**PR name:** `PR-F09-049 — feat(F-09, matching): grouped solver replay determinism + backtest.execution.groups producer`
**Goal:** Обеспечить детерминированный вывод grouped solver и публикацию в `backtest.execution.groups` для replay.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-010
**Estimated diff:** ≤ 80 lines

**Target files:**
- `cpp/matching/src/infra/backtest_execution_groups_producer.hpp` + `.cpp` — producer для `backtest.execution.groups` в replay-режиме
- `cpp/matching/src/domain/grouped_solver_bisection.cpp` — убедиться что sort legs по leg_id перед solve (детерминизм)
- `cpp/matching/tests/grouped_solver_replay_determinism_test.cpp` — `ReplayDeterministicGroupedExecution`

**Tests to add:**
- `ReplayDeterministicGroupedExecution`: N итераций того же ввода → идентичный ExecutionGroup (scale, leg fills, violated constraints, diagnostics)

**Acceptance criteria:**
- Replay в F-15 Backtest читает `backtest.execution.groups` (не `execution.groups`), изолированный от live данных
- Grouped solver не использует system time (time → инжектируется как параметр)

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R grouped_solver_replay_determinism_test` passing
- Зависит от: T-F09-044, T-F09-046, T-F09-023 (backtest.execution.groups топик)

**Rollback:** `git revert <sha>` безопасен

---

## Phase G: Venues / Execution Planning — External Legs

### T-F09-050: Execution Planning: ValidateExternalExecution + GetGroupedRoutingHints stubs

**PR name:** `PR-F09-050 — feat(F-09, execution-planning): ValidateExternalExecution + GetGroupedRoutingHints gRPC stubs`
**Goal:** TODO contract → stub gRPC service для execution-planning: валидация atomicityScope + routing hints.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-006
**Estimated diff:** ≤ 120 lines

**Target files:**
- `docs/06-api/grpc/execution-planning-validate-external.md` — создать TODO contract → spec
- `docs/06-api/grpc/execution-planning-routing-hints.md` — создать TODO contract → spec
- `cpp/venues/src/transport/execution_planning_grpc_handler.hpp` + `.cpp` — stub реализация: `ValidateExternalExecution` → `allowed=false` для `INTERNAL_BATCH`; `allowed=true` для `EXTERNAL_COMPENSATING`; `GetGroupedRoutingHints` → stub routing hints

**Tests to add:**
- `RejectExternalStrictAtomicWithoutNativeSupport`: `VENUE_NATIVE` + venue не поддерживает → REJECT

**Acceptance criteria:**
- `internal_batch` → `allowed=false, reason=external_legs_forbidden_for_internal_scope` (AC-F09-006)
- `venue_native` без native support → `cancelled_by_atomicity` (AC-F09-006)

**Definition of Done:**
- Build: зелёный
- TODO contracts трансформированы в spec docs
- Зависит от: T-F09-011

**Rollback:** `git revert <sha>` безопасен

**Risks:** Execution Planning может не существовать как отдельный сервис. В MVP — часть `venues` или отдельный модуль внутри `matching`.

---

### T-F09-051: Venues: external_compensating mode producer execution.intents per leg

**PR name:** `PR-F09-051 — feat(F-09, venues): external_compensating leg execution via execution.intents`
**Goal:** В режиме `external_compensating` matching публикует `ExecutionIntent` per external leg, venues исполняет best-effort, matching оценивает grouped result.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-006
**Estimated diff:** ≤ 100 lines

**Target files:**
- `cpp/matching/src/infra/external_leg_intent_producer.hpp` + `.cpp` — publish `execution.intents` per external leg с `mode=best_effort`, key = legId
- `cpp/matching/src/app/evaluate_grouped_external_result.hpp` + `.cpp` — агрегировать ExecutionReports от venues, проверить ratio/weights/spread constraints, выбрать groupStatus (filled/compensating/degraded)

**Tests to add:**
- `ExternalLegBestEffort`: один leg исполнен, другой нет → groupStatus=compensating
- `ExternalPartialFillCannotCommitAsAtomic` — negative

**Acceptance criteria:**
- При нарушении целевой структуры: publish `risk.alerts` с `EXTERNAL_COMPENSATION_REQUIRED`
- `ExecutionGroup.atomicityGuarantee` = `external_compensating` (не `strict_atomic`)

**Definition of Done:**
- Build: зелёный
- Зависит от: T-F09-045, T-F09-046

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-052: Venues: venue_native multi-leg order + atomicGuarantee check

**PR name:** `PR-F09-052 — feat(F-09, venues): venue_native multi-leg ExecutionIntent + atomicity guarantee check`
**Goal:** Для `venue_native` scope: venue-execution-adapter отправляет нативный multi-leg order; проверяет `atomicGuarantee=true` в ответе.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-006
**Estimated diff:** ≤ 80 lines

**Target files:**
- `cpp/venues/src/app/venue_native_multileg_executor.hpp` + `.cpp` — отправить `Native MultiLeg Order` с `requireAtomicExecution=true`; при `atomicGuarantee != true` → fail → publish `cancelled_by_atomicity`

**Tests to add:**
- `ExternalStrictAtomicRejected`: venue отвечает без guarantee → cancelled_by_atomicity

**Acceptance criteria:**
- `strict_atomic` без venue support → `GROUP_STATUS_CANCELLED_BY_ATOMICITY` + `risk.alerts GROUPED_ATOMICITY_FAILURE`
- Реальные venue credentials — не в репозитории (CLAUDE.md §22)

**Definition of Done:**
- Build: зелёный
- Tests: passing (mock venue)
- Зависит от: T-F09-050, T-F09-046

**Rollback:** `git revert <sha>` безопасен

---

## Phase H: Ledger — Grouped Postings

### T-F09-060: Ledger: execution.groups consumer + grouped postings idempotent

**PR name:** `PR-F09-060 — feat(F-09, ledger): consume execution.groups idempotent grouped postings`
**Goal:** Ledger потребляет `execution.groups`, применяет leg-level постинги, parent grouped summary, fees per leg, PnL, margin impact. Идемпотентно по `executionGroupId`.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-009
**Estimated diff:** ≤ 160 lines

**Target files:**
- `cpp/ledger/src/infra/execution_groups_consumer.hpp` + `.cpp` — Kafka consumer `execution.groups`; idempotency check по `(execution_group_id)` против `group_state_transitions`
- `cpp/ledger/src/app/apply_grouped_execution_use_case.hpp` + `.cpp` — per-leg debit/credit (base+quote assets); parent grouped summary; fees per leg; combined PnL; margin impact
- `cpp/ledger/tests/apply_grouped_execution_use_case_test.cpp` — `ApiToLedgerGroupedExecution`

**Non-target files:**
- Существующий ledger fill consumer (F-04) — не трогать

**Tests to add:**
- `DuplicateGroupEventNoDuplicateFills`: повторная доставка execution_group_id → no-op
- Basket 2 ноги: verifies balance debit for both legs + fee deducted
- `GROUP_STATUS_CANCELLED_BY_ATOMICITY`: нет постингов

**Acceptance criteria:**
- Ledger применяет `Decimal` (не float) для всех балансов (CLAUDE.md §9)
- Повторное применение того же `executionGroupId` — no-op (idempotent consumer pattern)
- `GROUP_STATUS_COMPENSATING` → compensation postings (reverse/hedge, идемпотентно по groupId)

**Definition of Done:**
- Build: зелёный
- Tests: `ctest -R apply_grouped_execution_use_case_test` passing
- Зависит от: T-F09-011, T-F09-020, T-F09-021, T-F09-046

**Rollback:** `git revert <sha>` — ledger применяет постинги; rollback самого кода безопасен, но уже применённые постинги из Kafka требуют отдельной down-миграции данных

**Risks:** Атомарность ledger операций по группе — все leg postings одной группы в одной PG-транзакции.

---

### T-F09-061: Ledger: compensation postings для external_compensating

**PR name:** `PR-F09-061 — feat(F-09, ledger): compensation postings for GROUP_STATUS_COMPENSATING`
**Goal:** Для `GROUP_STATUS_COMPENSATING/ROLLBACK_PENDING` применить compensation postings (reverse/hedge legs), идемпотентно по `groupId`.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-006, AC-F09-009
**Estimated diff:** ≤ 100 lines

**Target files:**
- `cpp/ledger/src/app/apply_grouped_execution_use_case.cpp` — добавить ветку COMPENSATING: reverse postings для failed legs; ROLLBACK_PENDING → mark for manual review + alert

**Tests to add:**
- `external_compensating`: leg A filled, leg B failed → compensation для leg B применена idempotently

**Acceptance criteria:**
- Compensation posting помечен в аудите с reason=`compensation_for_external_failure`

**Definition of Done:**
- Build: зелёный
- Зависит от: T-F09-060

**Rollback:** `git revert <sha>` безопасен

---

## Phase I: Observability

### T-F09-070: Observability: grouped metrics consumer от `execution.groups`

**PR name:** `PR-F09-070 — feat(F-09, observability): grouped metrics from execution.groups topic`
**Goal:** Observability потребляет `execution.groups` и пишет structured grouped metrics.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-005
**Estimated diff:** ≤ 100 lines

**Target files:**
- `cpp/observability/src/infra/execution_groups_observer.hpp` + `.cpp` — consume `execution.groups`; логировать: `group_solve_time_ms`, `rejected_groups` (CANCELLED_BY_ATOMICITY count), `degraded_groups` (DEGRADED count), `ratio_deviation_bps`, `orphan_leg_incidents` (нарушение strict_atomic без fills)
- ClickHouse ingestion: INSERT `grouped_execution_events`, `grouped_ratio_deviation` per consumed ExecutionGroup

**Tests to add:**
- DEGRADED group → `degraded_groups` counter incremented
- GROUP_STATUS_CANCELLED_BY_ATOMICITY → `orphan_leg_incidents` incremented (если были fills в ногах — не должно быть)

**Acceptance criteria:**
- Structured JSON logs с `parent_order_id`, `execution_group_id`, `batch_id`, `group_status`
- Consumer lag метрика доступна (CLAUDE.md §19)

**Definition of Done:**
- Build: зелёный
- Зависит от: T-F09-011, T-F09-020, T-F09-022

**Rollback:** `git revert <sha>` безопасен (observability не влияет на бизнес-состояние)

---

### T-F09-071: Observability: ClickHouse ingestion grouped_leg_fills и grouped_quality_metrics MV

**PR name:** `PR-F09-071 — feat(F-09, observability): ClickHouse ingest grouped_leg_fills + grouped_quality_metrics MV`
**Goal:** Ингестировать LegFill из `fills` топика в `grouped_leg_fills`; materialized view `grouped_quality_metrics` считает combined VWAP и IS.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-005, AC-F09-010
**Estimated diff:** ≤ 80 lines

**Target files:**
- `infra/clickhouse/init-f09.sql` — добавить Kafka Engine tables для `fills` → `grouped_leg_fills`; создать MV `grouped_quality_metrics` (если не было в T-F09-022)

**Tests to add:**
- `Testing/f09_olap_vwap_smoke.sh` — вставить 2 LegFill, проверить combined VWAP через SELECT

**Acceptance criteria:**
- `grouped_quality_metrics.combined_vwap` = sum(qty*price)/sum(qty) по формуле из CLAUDE.md §8.5
- `grouped_replay_results` не смешивается с live `grouped_leg_fills`

**Definition of Done:**
- Build: ClickHouse init зелёный
- Зависит от: T-F09-022, T-F09-046

**Rollback:** DROP TABLE IF EXISTS; безопасен

---

## Phase J: Frontend

### T-F09-080: Frontend: ComboOrder editor — legs, constraints, executionMode, preview

**PR name:** `PR-F09-080 — feat(F-09, frontend): ComboOrder editor with legs, constraints, grouped preview`
**Goal:** React-компонент создания ComboOrder: список ног, constraints, выбор executionMode/atomicityPolicy, grouped preview блок с binding leg.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-002
**Estimated diff:** ≤ 180 lines

**Target files:**
- `frontend/web/src/pages/ComboOrderPage.tsx` — форма создания
- `frontend/web/src/components/combo/LegEditor.tsx` — редактор одной ноги
- `frontend/web/src/components/combo/ConstraintEditor.tsx` — редактор constraint
- `frontend/web/src/components/combo/GroupedPreview.tsx` — отображение preview: expectedScale, bindingLeg, combinedVWAP, ratioDeviation
- `frontend/web/src/api/comboOrderApi.ts` — REST клиент для `POST /v1/combo-orders/preview` и `POST /v1/combo-orders`

**Non-target files:**
- `frontend/web/src/pages/OrdersPage.tsx` — не трогать

**Tests to add:**
- `frontend/web/src/components/combo/LegEditor.test.tsx` — unit тест: `orchestration_only` показывает warning banner

**Acceptance criteria:**
- При `executionMode=orchestration_only` — warning: "Ноги исполняются независимо. Веса/ratio/spread могут отклониться"
- Preview показывает binding leg подсвеченным

**Definition of Done:**
- Build: `npm run build` зелёный
- Tests: Jest тесты passing
- Зависит от: T-F09-036 (backend handlers)

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-081: Frontend: ComboOrder status page — group status, leg fills, ratio/spread deviation, fallback state

**PR name:** `PR-F09-081 — feat(F-09, frontend): ComboOrder status page with group fills and degraded states`
**Goal:** Страница статуса ComboOrder: parent status, leg статусы, execution scale, ratio deviation, fallback action, degraded/rollback_pending state.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-005, AC-F09-007, AC-F09-008
**Estimated diff:** ≤ 140 lines

**Target files:**
- `frontend/web/src/pages/ComboOrderDetailPage.tsx`
- `frontend/web/src/components/combo/ExecutionGroupCard.tsx` — отображение одного ExecutionGroup: scale, legResults, violatedConstraints, fallbackAction
- `frontend/web/src/components/combo/LegStatusBadge.tsx` — статус ноги с цветовой кодировкой

**Tests to add:**
- `ExecutionGroupCard.test.tsx` — DEGRADED статус показывает violatedConstraints
- OCO: при filled одной ветви — sibling показывается как cancelled

**Acceptance criteria:**
- `rollback_pending` и `compensating` явно различаются в UI
- `degraded` статус показывает `fallbackAction` и `ratioDeviationBps`

**Definition of Done:**
- Build: зелёный
- Зависит от: T-F09-080

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-082: Gateway: REST endpoints для combo-orders

**PR name:** `PR-F09-082 — feat(F-09, gateway): REST endpoints POST/GET/DELETE /v1/combo-orders`
**Goal:** HTTP gateway handlers для combo-orders: маршрутизация REST → gRPC OrderFlowService.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001
**Estimated diff:** ≤ 100 lines

**Target files:**
- `cpp/gateway/src/transport/combo_order_handler.hpp` + `.cpp` — `POST /v1/combo-orders`, `POST /v1/combo-orders/preview`, `GET /v1/combo-orders/{id}`, `DELETE /v1/combo-orders/{id}`, `POST /v1/batch-orders`; JSON → gRPC mapping
- `cpp/gateway/src/main.cpp` — зарегистрировать маршруты

**Tests to add:**
- `cpp/gateway/tests/combo_order_handler_test.cpp` — mock gRPC, verify JSON → proto mapping

**Acceptance criteria:**
- `POST /v1/combo-orders` возвращает 201 с `comboId` при approve
- `orchestration_only` warning присутствует в response body `{"warning": "..."}`
- Rate limiting применяется (наследуется от существующего gateway middleware)

**Definition of Done:**
- Build: зелёный
- Tests: passing
- Зависит от: T-F09-036

**Rollback:** `git revert <sha>` безопасен

---

## Phase K: Integration + E2E Tests

### T-F09-090: E2E: ApiToLedgerGroupedExecution — полный путь basket scalable_atomic

**PR name:** `PR-F09-090 — test(F-09, e2e): full path basket scalable_atomic from API to Ledger`
**Goal:** E2E тест: создать basket ComboOrder через REST, дождаться grouped execution, проверить balance changes в ledger.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-002, AC-F09-004, AC-F09-009
**Estimated diff:** ≤ 120 lines

**Target files:**
- `Testing/f09_basket_scalable_atomic_e2e.sh` — curl POST /v1/combo-orders; poll GET /v1/combo-orders/{id} до filled/partial; check balance via /v1/accounts/{id}

**Tests to add:** (это и есть тест)

**Acceptance criteria:**
- E2E проходит в `docker compose -f infra/docker-compose.dev.yml up` окружении
- Ratio deviation ≤ maxRatioDeviationBps

**Definition of Done:**
- `bash Testing/f09_basket_scalable_atomic_e2e.sh` зелёный
- Зависит от: все фазы D + F + H

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-091: Integration: PairStrictAtomicOneLegIlliquid — strict_atomic блокирует fills

**PR name:** `PR-F09-091 — test(F-09): pair strict_atomic illiquid leg → no fills, cancelled_by_atomicity`
**Goal:** Интеграционный тест: парная заявка strict_atomic, одна нога неликвидна → scale=0, fills=[], group=cancelled_by_atomicity.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-003
**Estimated diff:** ≤ 60 lines

**Target files:**
- `cpp/matching/tests/pair_strict_atomic_illiquid_test.cpp`

**Acceptance criteria:**
- `orphanLegs = 0`
- `executionScale = 0`
- `group_state_transitions` содержит запись `cancelled_by_atomicity`

**Definition of Done:**
- Build: зелёный; `ctest -R pair_strict_atomic_illiquid_test` passing
- Зависит от: T-F09-044, T-F09-045

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-092: Integration: OCOBranchActivationAndCancel

**PR name:** `PR-F09-092 — test(F-09): OCO branch activation idempotently cancels sibling`
**Goal:** Интеграционный тест OCO: активация одной ветви идемпотентно отменяет sibling.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-007
**Estimated diff:** ≤ 60 lines

**Target files:**
- `cpp/order_flow/tests/oco_branch_activation_test.cpp`

**Tests:** `ApplyOCOTransitions`, `DuplicateGroupEventNoDuplicateFills`

**Acceptance criteria:**
- Sibling branch переходит в `cancelled` ровно один раз (idempotency_key уникален)

**Definition of Done:**
- `ctest -R oco_branch_activation_test` passing
- Зависит от: T-F09-033, T-F09-045

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-093: Integration: BracketPartialEntryResize

**PR name:** `PR-F09-093 — test(F-09): bracket order partial entry → exit legs resized to filled qty`
**Goal:** Bracket order: entry частично исполнен → Q_tp = Q_sl = Q_entry_filled.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-008
**Estimated diff:** ≤ 60 lines

**Target files:**
- `cpp/matching/tests/bracket_partial_entry_resize_test.cpp`

**Tests:** `ResizeBracketExits`, `BracketPartialEntryResize`

**Acceptance criteria:**
- `Q_tp + Q_sl ≤ Q_entry_filled` после resize
- При Q_entry_filled = 0 exit ветви остаются в `waiting_for_trigger`

**Definition of Done:**
- `ctest -R bracket_partial_entry_resize_test` passing
- Зависит от: T-F09-045

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-094: Integration: SpreadConstraintPassFail

**PR name:** `PR-F09-094 — test(F-09): spread constraint pass and fail scenarios`
**Goal:** Spread order: исполнение только при c_g^T * P в допустимом диапазоне; при нарушении → waiting_next_batch.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-001, AC-F09-005
**Estimated diff:** ≤ 60 lines

**Target files:**
- `cpp/matching/tests/spread_constraint_test.cpp`

**Tests:** `SpreadConstraintPassFail`, `StaleReferencePriceNoSpreadExecution`

**Acceptance criteria:**
- При stale reference price → execution BLOCKED, reason logged
- При c_g^T P вне [L_g, U_g] → группа не исполняется (strict_atomic/scalable_atomic)

**Definition of Done:**
- `ctest -R spread_constraint_test` passing
- Зависит от: T-F09-044 (MVP-3 scope если spread QP solver не реализован — тест помечен TODO для MVP-3)

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-095: Integration: ReplayDeterministicGroupedExecution

**PR name:** `PR-F09-095 — test(F-09): replay deterministic grouped execution (AC-F09-010)`
**Goal:** F-15 Replay воспроизводит grouped execution детерминированно: тот же ExecutionGroup, leg fills, status transitions, solver diagnostics при одном входе.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-010
**Estimated diff:** ≤ 80 lines

**Target files:**
- `Testing/f09_replay_determinism_e2e.sh` — создать ComboOrder, дождаться grouped execution, сохранить ExecutionGroup JSON; запустить replay session (F-15 API); сравнить ExecutionGroup из backtest.execution.groups с live результатом

**Acceptance criteria:**
- `executionGroupId`, `executionScale`, `legResults[]`, `ratioDeviationBps`, `solverDiagnostics` идентичны при replay

**Definition of Done:**
- `bash Testing/f09_replay_determinism_e2e.sh` зелёный
- Зависит от: T-F09-049, T-F09-023, F-15 replay service

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-096: Negative integration: ExternalPartialFillCannotCommitAsAtomic + MissingLegFillRejectStrictGroup

**PR name:** `PR-F09-096 — test(F-09, negative): external partial fill not atomic + missing leg rejects strict`
**Goal:** Negative test suite: внешнее частичное исполнение не может быть помечено strict_atomic; отсутствие fill у обязательной ноги → reject strict group.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-003, AC-F09-006
**Estimated diff:** ≤ 80 lines

**Target files:**
- `cpp/matching/tests/negative_atomicity_test.cpp` — `ExternalPartialFillCannotCommitAsAtomic`, `MissingLegFillRejectStrictGroup`, `IndependentSplitCannotBeStrictAtomic`

**Acceptance criteria:**
- Все 3 negative теста бросают ожидаемую ошибку / возвращают ожидаемый статус
- `IndependentSplitCannotBeStrictAtomic`: попытка установить `strict_atomic` для `orchestration_only` → REJECT на уровне `CreateComboOrder`

**Definition of Done:**
- `ctest -R negative_atomicity_test` passing
- Зависит от: T-F09-031, T-F09-044, T-F09-051

**Rollback:** `git revert <sha>` безопасен

---

## Phase L: MVP-6 Compensation Resolution (slice 3b — MONEY)

> Источник: ADR-039 (operator-driven resolution) + **ADR-040** (размещение endpoint
> в order_flow, matching экспонирует `CompensationService` gRPC; владение
> `combo_compensations` остаётся за matching). Предшествующие срезы готовы: DDL
> (slice 1), repo `ResolvePending`/`ListPending` (slice 2), pure `ComputeReversals`
> (slice 3a). Ниже — реализация operator-authorized resolution до денег.

### T-F09-063: ADR-040 — размещение endpoint + cross-service контракт

**PR name:** `PR-F09-063 — docs(adr): ADR-040 compensation resolution cross-service placement`
**Goal:** Зафиксировать решение «endpoint в order_flow, matching экспонирует
`CompensationService`» (выбор зафиксирован 2026-06-11).
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-006 (внешние/компенсация)
**Estimated diff:** ≤ 130 lines (docs-only)

**Target files:**
- `docs/03-architecture/adr/ADR-040-compensation-resolution-cross-service.md` — создать ✅

**Acceptance criteria:**
- ADR содержит контекст (конфликт §10.3 vs §14/§17), решение, альтернативы,
  последствия, обратимость
- Ссылается на ADR-039 §5

**Definition of Done:** docs-only, сборка не требуется. **Статус: ✅ выполнено.**

**Rollback:** `git revert <sha>` безопасен

---

### T-F09-064: Proto `compensation.proto` — CompensationService gRPC

**PR name:** `PR-F09-064 — feat(F-09, proto): add matching CompensationService (List/Get/Resolve)`
**Goal:** Материализовать gRPC-контракт для read+resolve компенсаций (matching-side).
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-006, AC-F09-009
**Estimated diff:** ≤ 90 lines

**Target files:**
- `contracts/proto/fob/matching/v1/compensation.proto` — создать: `service
  CompensationService` с rpc `ListPendingCompensations`, `GetPendingCompensation`,
  `ResolvePending`; messages `PendingCompensation` (compensation_id, parent_order_id,
  leg_id, reason, internal_filled_qty:Decimal), Request/Response для 3 методов;
  `ResolvePendingRequest{compensation_id, action, operator_id, resolving_ref}`
- `contracts/CMakeLists.txt` — добавить файл
- `docs/06-api/grpc/matching-compensation-service.md` — контракт-doc

**Non-target files:**
- `contracts/proto/fob/matching/v1/solver.proto` — не трогать (отдельный сервис)

**Acceptance criteria:**
- `internal_filled_qty` — тип `fob.common.v1.Decimal` (не double, §9)
- `cmake --build build --target contracts_proto` зелёный
- backward-compat: новый сервис аддитивен

**Definition of Done:** protoc зелёный; контракт-doc создан. Зависит от: T-F09-063.
**Статус: ✅ выполнено.** `compensation.proto` создан, `protoc` зелёный локально
(импорты common разрешаются); контракт-doc `matching-compensation-service.md` создан.
`contracts/CMakeLists.txt` правки не требует (`GLOB_RECURSE *.proto` подхватывает файл).

**Rollback:** `git revert <sha>` безопасен (аддитивный proto)

**Risks:** action — `string` (reverse_internal|retry_external|accept) для читаемости,
валидация на стороне сервера.

---

### T-F09-065: matching transport — CompensationService gRPC server

**PR name:** `PR-F09-065 — feat(F-09, matching): CompensationService gRPC over existing repo`
**Goal:** Поднять gRPC-сервис поверх `PostgresComboCompensationRepository`
(`ListPending`/`ResolvePending` уже есть; добавить `GetPendingCompensation`).
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-006, AC-F09-009
**Estimated diff:** ≤ 160 lines

**Target files:**
- `cpp/matching/src/transport/grpc_compensation_service.{hpp,cpp}` — 3 rpc → repo
- `cpp/matching/src/infra/postgres_combo_compensation_repository.{hpp,cpp}` —
  добавить `std::optional<PendingCompensation> GetPending(compensation_id)`
- `cpp/matching/src/main.cpp` — зарегистрировать сервис на существующем gRPC-сервере
  (рядом с `grpc_isolation_matching_service`)
- `cpp/matching/tests/grpc_compensation_service_test.cpp` — list/get/resolve, idempotent

**Non-target files:**
- `cpp/matching/src/app/matching_loop.cpp` — не трогать (compensation consumer уже там)

**Acceptance criteria:**
- `ResolvePending` идемпотентен (повтор pending→resolved — no-op, true только при
  фактическом переходе)
- `ListPendingCompensations` возвращает все pending
- gRPC-регистрация не ломает `Solver`/isolation service

**Definition of Done:** `ctest -R grpc_compensation_service_test` (live PG) зелёный.
Зависит от: T-F09-064.
**Статус: ✅ выполнено (verified live).** `grpc_compensation_service.{hpp,cpp}` (3 rpc),
репо `GetPending`+`ListPending(parent)`, регистрация на isolation gRPC-сервере (порт
50053, gated на `MATCHING_POSTGRES_DSN`). `matching_grpc_compensation_service_test`
Passed против live PG (cex_net); полный `matching` executable собирается. Решён риск
«нет постоянного gRPC-сервера»: isolation server поднимается всегда.

**Rollback:** `git revert <sha>` безопасен (новый сервис, репо-метод аддитивен)

**Risks:** matching может не иметь постоянного gRPC-сервера в проде — проверить, что
`grpc_isolation_matching_service` поднимается всегда, а не только в replay.

---

### T-F09-066: Перенести `ComputeReversals` в `cpp/common` (shared, ADR-040 §4)

**PR name:** `PR-F09-066 — refactor(F-09): relocate ComputeReversals to cpp/common`
**Goal:** Сделать pure `ComputeReversals` доступной order_flow без matching-зависимости.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-009
**Estimated diff:** ≤ 80 lines (перемещение + правка include)

**Target files:**
- `cpp/common/src/domain/compensation_reversal.{hpp,cpp}` — перенести из
  `cpp/matching/src/domain/compensation_reversal*`
- `cpp/matching/...` — обновить include на common; matching-тест оставить зелёным
- `cpp/common/tests/compensation_reversal_test.cpp` — перенести unit-тесты slice 3a

**Acceptance criteria:**
- Функция остаётся pure (без IO/Kafka/DB), сигнатура `internal_legs → ReversalOrder[]`
  неизменна
- matching и order_flow оба линкуются с common-версией
- Существующие unit-тесты slice 3a зелёные после переезда

**Definition of Done:** `cmake --build build -j` + `ctest -R compensation_reversal`
зелёные. Зависит от: T-F09-063.
**Статус: ✅ выполнено (verified live).** `ComputeReversals`+`ReversalLeg`/`ReversalOrder`
перенесены `cpp/matching/src/domain/` → `cpp/common` (namespace `cex::matching::domain`
→ `cex::common`); тест → `cex_common_compensation_reversal_test` (Passed); matching
собирается без старых файлов. Старые файлы и matching CMake-записи удалены.

**Rollback:** `git revert <sha>` безопасен (чистый рефактор, поведение неизменно)

**Risks:** Циклы в CMake — common не должен зависеть от matching (направление верное).

---

### T-F09-067: order_flow — matching compensation client + ResolveCompensation UseCase + operator gRPC

**PR name:** `PR-F09-067 — feat(F-09, order-flow): operator ResolveCompensation (reverse_internal money path)`
**Goal:** Operator-authorized endpoint: read pending (matching gRPC) → reverse_internal
считает реверс из `combo_order_legs` → `CreateFlowOrder` → matching `ResolvePending`.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-006, AC-F09-009
**Estimated diff:** ≤ 220 lines

**Target files:**
- `cpp/order_flow/src/infra/matching_compensation_client.{hpp,cpp}` — gRPC client к
  `CompensationService` (Get/List/Resolve), паттерн `risk_client`/`ledger_client`
- `cpp/order_flow/src/app/resolve_compensation_use_case.{hpp,cpp}` — оркестрация
  (ADR-040 §3): get → load internal legs (combo repo, `filled_cum`) → `ComputeReversals`
  → `CreateFlowOrder` (client_order_id = compensation_id+leg, идемпотентно) → ResolvePending
- `cpp/order_flow/src/transport/grpc_order_flow_service.{hpp,cpp}` — rpc
  `ResolveCompensation` с operator-auth guard (по образцу `SetKillSwitch`)
- `cpp/order_flow/tests/resolve_compensation_use_case_test.cpp` — reverse_internal,
  accept, идемпотентность ретрая, retry_external (re-emit intent)
- `docs/06-api/grpc/order-flow-resolve-compensation.md` — контракт-doc

**Non-target files:**
- `cpp/order_flow/src/infra/postgres_combo_order_repository.*` — добавить только read
  `filled_cum` по combo, не менять write-путь
- `cpp/ledger/` — не трогать (реверс идёт штатным pipeline)

**Acceptance criteria:**
- Деньги идут ТОЛЬКО через `CreateFlowOrder` pipeline (нет прямого ledger-мутирования)
- `reverse_internal`: создаётся FlowOrder противоположной стороны, qty = Σ filled_cum
  внутренних ног; `resolving_ref` = id реверсивной заявки; затем `ResolvePending(resolved)`
- `accept` → `ResolvePending(accept)` без создания ордера (cancelled)
- Ретрай endpoint идемпотентен (повторный вызов не создаёт дублей FlowOrder и не
  переоткрывает resolved)
- operator-auth guard отклоняет неавторизованный вызов (как kill-switch)

**Definition of Done:** `ctest -R resolve_compensation_use_case_test` зелёный; контракт-doc
создан. Зависит от: T-F09-064, T-F09-065, T-F09-066.
**Статус: ✅ выполнено (unit verified live).** proto `ResolveCompensation` rpc +
messages; `matching_compensation_client` (Get/Resolve); `resolve_compensation_use_case`
(std::function-порты: get→reverse_internal/accept/retry_external→resolve); repo
`LoadInternalFilledLegs` (internal-ноги filled_cum>0 + owner; локальный NUMERIC-парсер);
DTO в `combo_reversal_context.hpp` (без pqxx); operator-auth как SetKillSwitch (operator_id
+ audit); idem `client_order_id=comp:leg`; band/rate из исходной ноги; gRPC-метод +
wiring (`MATCHING_GRPC_TARGET`, gated на DSN); контракт-doc. `order_flow_resolve_comp_tests`
Passed (6 кейсов) + order_flow executable собирается. retry_external → NOT_IMPLEMENTED (MVP-7).

**Rollback:** `git revert <sha>` безопасен; за operator-auth флагом можно отключить.

**Risks:** partial-fill математика реверса (несимметричный fill ног) — в slice 3b
полный объём `filled_cum`; сложные partial-кейсы помечены deferred MVP-7 (ADR-039).

---

### T-F09-068: E2E — operator resolves pending compensation → reverse → ledger

**PR name:** `PR-F09-068 — test(F-09, e2e): operator compensation resolution reverses internal exposure`
**Goal:** Полный путь: внешняя нога combo отклонена → pending compensation →
operator `ResolveCompensation(reverse_internal)` → реверсивная FlowOrder → ledger
разворачивает позицию → `combo_compensations.status = resolved`.
**Linked feature:** [F-09](../02-system/features/F-09-batch-combo-orders/feature.yaml)
**Linked AC:** AC-F09-006, AC-F09-009
**Estimated diff:** ≤ 120 lines (Testing/ скрипт)

**Target files:**
- `Testing/f09_compensation_resolution_e2e.sh` — combo (internal+external) → rejecting
  venue → pending → gRPC ResolveCompensation → assert reversing FlowOrder + ledger
  delta + status=resolved
- `docs/10-testing/features/F-09-test-plan.md` — добавить кейс

**Acceptance criteria:**
- Требует «отклоняющего» venue-sim (Трек 2): live-провал внешней ноги
- Повторный ResolveCompensation — no-op (идемпотентность на E2E-уровне)
- Позиция combo после реверса нейтрализована по внутренним ногам

**Definition of Done:** скрипт зелёный на dev-стеке. Зависит от: T-F09-067 + Трек 2
(rejecting venue для live-провала).
**Статус: ✅ выполнено (PASSED live).** `Testing/f09_compensation_resolution_e2e.sh`
зелёный на dev-стеке. Полная цепочка проверена LIVE: combo (BTC internal buy +
ETH external sell→binance) → internal наполнена (scalable_atomic, filled_cum=0.002)
→ binance REJECT дискретного ETH-ордера → matching pending compensation →
ResolveCompensation(reverse_internal) applied=true + reversingOrderIds → reversing
FlowOrder persisted → status=resolved → re-resolve no-op (идемпотентно).
**Потребовало venue/matching-фиксов (вне resolution-кода, но нужны для live):**
(a) `combo_external_routing.cpp BuildExternalIntent` ставит `intent.venue`=primary
preference (дискретный ордер на конкретную биржу — иначе legacy round-robin минует
venue-sim); (b) EVC simulate (`cex_ws_rest_adapter`) умеет REJECT по
`simulate_reject_symbols` (env `BINANCE_SIMULATE_REJECT_SYMBOLS=ETH/USDT`) —
моделирует отказ реальной биржи; (c) `resolve_compensation_use_case` ставит
детерминированный `order_id` реверс-ордера (gateway генерит id, а UC зовётся
напрямую); (d) combo для E2E = `scalable_atomic` (EXTERNAL_COMPENSATING политика
в solver'е = kBlocked, не наполняет — см. grouped_solver_bisection.cpp:142);
(e) compose env `F09_EXTERNAL_COMPENSATING_ENABLED=true` на order_flow.

**Rollback:** `git revert <sha>` безопасен (тест-скрипт)

**Risks:** Текущий venue-sim заполняет все intents → нужен режим reject (см. Трек 2,
live external-leg failure E2E). До этого — unit/integration покрытие (T-F09-067).

---

## Phase M: MVP-7 Auto-policy Resolution (ADR-041)

> Авто-резолв pending-компенсаций по правилам, БЕЗ оператора — аддитивный слой над
> slice-3b (тот же money-путь). Money-safety: pure guardrail-функция + circuit-breaker;
> default OFF. ADR-041.

### T-F09-072: ADR-041 + AutoResolvePolicy (pure guardrails)

**Статус: ✅ выполнено (verified).** `docs/03-architecture/adr/ADR-041-auto-policy-compensation-resolution.md`
(operator-driven остаётся; авто — только `reverse_internal` в пределах guardrails,
иначе escalate; никогда авто-accept/retry; circuit-breaker по оконному notional;
default OFF). `cpp/order_flow/src/app/auto_resolve_policy.{hpp,cpp}` — чистая
`EvaluateAutoResolve(candidate, config, window)` (enabled / terminal-reason / min_age /
nothing_to_reverse / notional-cap / window-count / window-notional). 11-кейсовый
unit-тест `order_flow_auto_resolve_policy_tests` PASSED. README ADR обновлён (+040/041).

### T-F09-073: created_at_ms (age-gate) + ListPending client

**Статус: ✅ выполнено.** `compensation.proto` `PendingCompensation += created_at_ms`
(epoch-ms); matching repo (`EXTRACT(EPOCH FROM created_at)*1000`) + grpc ToProto;
`MatchingCompensationClient += ListPending`. Сборка matching+order_flow зелёная.

### T-F09-074: AutoResolveLoop + wiring + live verify

**Статус: ✅ выполнено (live).** `auto_resolve_loop.{hpp,cpp}` — периодический поток
(gated `F09_AUTO_RESOLVE_ENABLED`): ListPending → оценка notional
`Σ filled_cum·mid(p_low,p_high)` → `EvaluateAutoResolve` → при auto вызывает
`ResolveCompensationUseCase(reverse_internal, operator_id="auto:reverse_internal")` +
скользящее окно (circuit-breaker). main.cpp wiring + env (compose order_flow). **Live:**
deployed auto ON — loop авто-разрешил 2 eligible (notional_est=145<cap) → status=resolved,
operator_id=auto:reverse_internal, reversing FlowOrders persisted; notional=0 →
escalate (pending, fail-safe). Открытый gap (отдельно, MVP-5): external reject не
маркирует ногу терминальной → matching re-routes каждый батч → новые компенсации.

---

## Recommended MVP Increments

| MVP | Scope | Задачи | Когда |
| --- | --- | --- | --- |
| **MVP-1** `orchestration_only` | Parent object, группировка независимых FlowOrder, групповая отмена, UI. Без grouped solver. | T-F09-001, T-F09-010, T-F09-012, T-F09-013, T-F09-020, T-F09-021, T-F09-030, T-F09-031, T-F09-033, T-F09-034, T-F09-035, T-F09-036, T-F09-082, T-F09-080 (partial) | Первый инкремент |
| **MVP-2** `scalable_atomic` ratio/basket internal | Grouped solver (bisection), feasible caps, scalable_atomic + strict_atomic для internal_batch. Ledger grouped postings. | T-F09-032, T-F09-040, T-F09-041, T-F09-043, T-F09-044, T-F09-045, T-F09-046, T-F09-047, T-F09-048, T-F09-049, T-F09-060, T-F09-070, T-F09-090, T-F09-091 | После MVP-1 |
| **MVP-3** spread/factor/budget/risk + strict_atomic | Линейная система (A_g e=b_g α) для spread/factor constraints. QP solver (OSQP/Eigen). Strict_atomic polished. | T-F09-094 + solver extension в T-F09-044 | После MVP-2 |
| **MVP-4** OCO/bracket + observability + E2E | OCO transitions, bracket resize, full observability, replay determinism. | T-F09-022, T-F09-023, T-F09-071, T-F09-081, T-F09-092, T-F09-093, T-F09-095, T-F09-096 | После MVP-3 |
| **MVP-5** external/compensating | venue_native, external_compensating, compensation ledger, venues stubs → real. | T-F09-050, T-F09-051, T-F09-052, T-F09-061 | После MVP-4 или параллельно |
| **MVP-6** compensation resolution (operator-driven) | ADR-039/040. DDL+repo (slice1/2 ✅), pure ComputeReversals (slice3a ✅), operator gRPC ResolveCompensation→reverse_internal money path (slice3b ✅ live), F-16 console (slice4 ✅ live). | T-F09-063..068 (slice3b), slice4=frontend | ✅ После MVP-5 |
| **MVP-7** auto-policy resolution | ADR-041 (money-guardrails + circuit-breaker, default OFF). pure AutoResolvePolicy + AutoResolveLoop (order_flow), reuse slice-3b money path, operator_id="auto:...". ✅ live-verified. | T-F09-072..074 (Phase M) | ✅ После MVP-6 |

---

## Effort Estimates

| Фаза | Задач | Оценка (человеко-часы) |
| --- | --- | --- |
| A: Test Plan | 1 | 2 |
| B: Proto contracts | 4 | 6 |
| C: Data / Infra | 4 | 8 |
| D: order_flow | 7 | 20 |
| E: Risk | 2 | 6 |
| F: Matching solver | 7 | 30 |
| G: Venues/Execution | 3 | 8 |
| H: Ledger | 2 | 8 |
| I: Observability | 2 | 6 |
| J: Frontend | 3 | 12 |
| K: Tests E2E | 7 | 14 |
| **Итого** | **42** | **~120 ч** |

> MVP-1 (orchestration_only): ~30 ч
> MVP-2 (scalable_atomic internal): +35 ч
> MVP-3..5: +55 ч

---

## Out of Scope (IN-011 §19)

Следующее **не входит** в F-09 ни в каком MVP-срезе:

- Cross-venue true atomic execution без native venue support
- Complex options strategy solver (Greeks-based)
- Non-linear constraints (нелинейные Greeks-based)
- RL-based routing (обучение с подкреплением)
- Full rollback уже совершённых внешних сделок (venue не поддерживает)
- Prime-broker style settlement netting

---

## Open Questions (требуют решения до T-F09-044)

| # | Вопрос | Влияние | Срок |
| --- | --- | --- | --- |
| OQ-1 | ✅ **РЕШЕНО — [ADR-034](../03-architecture/adr/ADR-034-grouped-constraint-solver.md).** MVP-2/3: bisection (closed-form) + feasibility-gate constraint evaluator (block/degrade). Полный QP (`A_g e=b_g α`) **отложен** до реального спроса; при внедрении — OSQP за `IGroupedSolver` в детерминированном режиме (replay). | T-F09-044, T-F09-094 | закрыто ADR-034 |
| OQ-2 | `combo_order_legs` vs `flow_order_legs` — рекомендация oltp-schema.md: отдельная таблица. Окончательно закрыть как ADR-decision в ADR-032. | T-F09-021, T-F09-034; влияет на F-02/F-04 repos | До T-F09-021 |
| OQ-3 | `fills` топик расширение (FlowFill tags 20+) — breaking change assessment для существующих consumers без перекомпиляции? | T-F09-013 | До T-F09-013 |
| OQ-4 | Интеграция `SolveGroupedBatch` с F-04 `RunBatchUseCase`: отдельный timer или shared cycle? | T-F09-045 performance | До T-F09-045 |
| OQ-5 | Execution Planning — отдельный сервис или модуль matching/venues? | T-F09-050; влияет на CMakeLists и docker-compose | До T-F09-050 |

---

## Definition of Done (Feature Gate)

F-09 считается реализованной корректно если:

- [ ] Оба режима `orchestration_only` и `multileg_vector_solver` работают (AC-F09-001, IN-011 §21)
- [ ] `BasketOrder` с жёсткими weights не выходит за `maxWeightDeviationBps` (AC-F09-002)
- [ ] `strict_atomic` при недоступной ноге: scale=0, fills=[], orphanLegs=0 (AC-F09-003)
- [ ] `scalable_atomic`: e_g = α_g * ρ_g, ratio deviation ≤ tolerance (AC-F09-004)
- [ ] `best_effort` фиксирует violatedConstraints + degraded status (AC-F09-005)
- [ ] Внешние ноги без native support не маркируются strict_atomic (AC-F09-006)
- [ ] OCO: cancel sibling идемпотентен (AC-F09-007)
- [ ] Bracket: exit legs активируются на Q_entry_filled (AC-F09-008)
- [ ] Ledger: grouped postings идемпотентны по executionGroupId (AC-F09-009)
- [ ] Grouped execution детерминированно воспроизводится в F-15 Replay (AC-F09-010)
- [ ] E2E тест `f09_basket_scalable_atomic_e2e.sh` зелёный в docker compose окружении
- [ ] Все TODO contracts (risk-pre-trade-check-group.md, execution-planning-validate-external.md, execution-planning-routing-hints.md) заменены спецификациями
- [ ] `docs/traceability/coverage-matrix.md` F-09 строка обновлена до ✅ contracts, ✅ data, ✅ tests
- [ ] `docs/02-system/features/F-09-batch-combo-orders/feature.yaml` поле `status` = `implemented`

---

## Traceability

| AC | Tasks | Tests |
| --- | --- | --- |
| AC-F09-001 | T-F09-010..013, T-F09-031, T-F09-036, T-F09-043..048 | `SolveGroupedBatchScalableAtomic`, `IndependentSplitCannotBeStrictAtomic` |
| AC-F09-002 | T-F09-032, T-F09-041, T-F09-044 | `BasketTargetWeightsDeviation`, `BasketScalableAtomicOneLegCapped` |
| AC-F09-003 | T-F09-044, T-F09-045, T-F09-091 | `SolveGroupedBatchStrictAtomic`, `MissingLegFillRejectStrictGroup`, `PairStrictAtomicOneLegIlliquid` |
| AC-F09-004 | T-F09-043, T-F09-044, T-F09-045 | `SolveGroupedBatchScalableAtomic`, `BasketScalableAtomicOneLegCapped` |
| AC-F09-005 | T-F09-044, T-F09-045, T-F09-070 | `ApplyBestEffortPolicy`, `ExternalLegBestEffort` |
| AC-F09-006 | T-F09-040, T-F09-050, T-F09-051, T-F09-052, T-F09-096 | `RejectExternalStrictAtomicWithoutNativeSupport`, `ExternalStrictAtomicRejected`, `ExternalPartialFillCannotCommitAsAtomic` |
| AC-F09-007 | T-F09-033, T-F09-045, T-F09-092 | `ApplyOCOTransitions`, `OCOBranchActivationAndCancel`, `DuplicateGroupEventNoDuplicateFills` |
| AC-F09-008 | T-F09-045, T-F09-093 | `ResizeBracketExits`, `BracketPartialEntryResize` |
| AC-F09-009 | T-F09-060, T-F09-061, T-F09-090 | `ApiToLedgerGroupedExecution`, `DuplicateGroupEventNoDuplicateFills` |
| AC-F09-010 | T-F09-049, T-F09-095 | `ReplayDeterministicGroupedExecution` |
