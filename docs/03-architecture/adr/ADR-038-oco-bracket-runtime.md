---
id: ADR-038
status: accepted
date: 2026-06-10
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - docs/03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md
  - docs/03-architecture/adr/ADR-032-parent-child-order-model.md
  - docs/03-architecture/adr/ADR-033-execution-groups-topic.md
  - cpp/matching/src/domain/child_graph_transitions.cpp
  - cpp/matching/src/app/solve_grouped_batch_use_case.cpp
  - cpp/matching/src/infra/active_grouped_orders_loader.cpp
  - infra/postgres/init.sql
source: IN-011 (F-09 v2) §11.6; F-09 v2 system-impact §3.7, §3.9
---

# ADR-038: OCO/bracket runtime — execution-семантика и персистенс leg-переходов

> **Note:** ранее имел номер ADR-035 (коллизия с ADR-035 FOB Solver Mathematical
> Foundation из IN-012, закоммиченным раньше) → переномерован в ADR-038.

## Контекст

MVP-2/3 реализовали grouped solver для basket/ratio (все ноги исполняются вместе
пропорционально) + constraint-gate. OCO и bracket — **другая семантика**:

- **OCO** (one-cancels-other): ноги — *альтернативы*. Исполнение одной ветви
  должно **отменять** остальные (IN-011 §11.6, AC-F09-007).
- **Bracket**: entry + take-profit + stop-loss. TP/SL — *exit*-ноги, активируются
  **после** заполнения entry, с объёмом `Q_tp = Q_sl = Q_entry^filled`
  (AC-F09-008).

Доменные функции `ApplyOCOTransitions` / `ResizeBracketExits`
(`child_graph_transitions.cpp`) реализованы и протестированы, но **не подключены
к runtime**. Подключение блокируют два открытых вопроса:

1. **Персистенс leg-переходов.** `group_state_transitions.group_id` — `NOT NULL`
   FK на `execution_groups(execution_group_id)`. Leg-level cancel/resize нельзя
   записать «вне» execution_group.
2. **Семантика vs basket-солвер.** Текущий solver исполнил бы **все** ноги OCO
   (как basket) — это нарушает «one-cancels-other». Bracket-солвер не должен
   исполнять TP/SL до заполнения entry.

## Решение

### 1. Персистенс leg-переходов

Leg-переход всегда **вызван исполнением** в каком-то batch, который произвёл
`ExecutionGroup`. Поэтому `group_state_transitions.group_id` = `execution_group_id`
**того batch-цикла**, в котором сработал переход. FK удовлетворён без изменения
схемы. Идемпотентность — по существующему `idempotency_key`
(`parent:source->target:action`, уже в `GraphTransition`). Доп. таблица не нужна.

### 2. Статусная модель ног управляет тем, что видит солвер

Loader загружает в активный набор солвера **только ноги в статусе `active`**.
OCO/bracket выражаются через статусы ног + граф (`conditional_links`):

| Тип | Начальные статусы ног | Переход |
| --- | --- | --- |
| **OCO** | выбранная ветвь `active`, сиблинги `active` (конкурируют) | при первом fill любой ветви → `ApplyOCOTransitions` переводит сиблингов в `cancelled` (идемпотентно), они выпадают из active-набора следующих batch |
| **Bracket** | entry `active`; TP/SL `waiting_for_trigger` | при fill entry → `ResizeBracketExits` ставит `Q_tp=Q_sl=Q_entry^filled` и переводит TP/SL в `active` (активируются для следующих batch) |

**OCO в MVP — eventual-consistency, не атомарный one-branch:** один batch может
частично исполнить >1 ветви до отмены сиблингов; ratio/atomicity тут не
гарантируются (что согласуется с тем, что OCO — это `best_effort`-подобный режим,
а не strict). Атомарный one-branch (выбор единственной ветви до solve) — MVP-4.1,
если потребуется.

### 3. Runtime-шаг в matching_loop (после grouped solve)

После `SolveGroupedBatch` и публикации `ExecutionGroup`:
1. построить `ComboGroupState` из загруженных ног (статус/filled) + рёбер графа;
2. применить filled-ноги текущего batch к состоянию;
3. `ApplyOCOTransitions` + `ResizeBracketExits` → список `GraphTransition`;
4. в одной PG-транзакции: `UPDATE combo_order_legs` (status/q_max) +
   `INSERT group_state_transitions` (`group_id = execution_group_id`,
   `ON CONFLICT (idempotency_key) DO NOTHING`).

Аддитивно к basket-пути; группы без `conditional_links` (pair/basket) — no-op.

## Альтернативы

| Вопрос | Вариант | Вердикт |
| --- | --- | --- |
| Персистенс | nullable FK / отдельная таблица `combo_leg_transitions` | отклонено — переход всегда привязан к batch-execution_group; лишняя сложность |
| Персистенс | **group_id = триггерящий execution_group_id (выбрано)** | принято — FK как есть |
| OCO-семантика | basket-solve + cancel сиблингов (eventual) **(выбрано для MVP)** | принято — просто, согласуется с best_effort |
| OCO-семантика | branch-selection до solve (атомарный one-branch) | отложено в MVP-4.1 (нужна метрика выбора ветви) |
| Bracket | TP/SL через статус `waiting_for_trigger` → `active` по entry fill **(выбрано)** | принято |

## Последствия

- Подключение использует готовые `child_graph_transitions` + статусную модель ног;
  схема БД не меняется.
- OCO в MVP — eventual (возможно частичное исполнение нескольких ветвей до
  отмены) — **должно быть честно отражено** в `execution_guarantees`
  (AC-F09-011): OCO ≈ best_effort.
- Loader должен фильтровать active-ноги (status='active') для солвера и
  загружать `conditional_links`.
- Атомарный one-branch OCO и полноценные триггеры (`trigger_condition`) — MVP-4.1.

## Обратимость

Высокая. Runtime-шаг аддитивен и gated (только для групп с `conditional_links`);
basket/ratio-путь не затрагивается. Можно убрать шаг без изменения контрактов.
Переход на атомарный one-branch — замена pre-solve фильтра, изолированно.
