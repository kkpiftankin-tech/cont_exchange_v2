---
id: ADR-036
status: accepted
date: 2026-06-10
owners:
  - architecture
  - core-team
related:
  - docs/03-architecture/adr/ADR-038-oco-bracket-runtime.md
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - cpp/matching/src/domain/child_graph_transitions.cpp
  - cpp/matching/src/domain/trigger_condition.cpp
  - cpp/order_flow/src/app/create_combo_order_use_case.cpp
source: ADR-038 §2 (deferred MVP-4.1); F-09 v2 §11.6
---

# ADR-036: Atomic one-branch OCO через conditional-ветви

## Контекст

ADR-038 ввёл OCO runtime в форме **eventual-consistency**: все ветви OCO активны,
basket-solver исполняет их вместе, после первого fill `ApplyOCOTransitions`
отменяет сиблингов. Один batch может частично исполнить >1 ветви до отмены. ADR-038
отложил **атомарный one-branch OCO** (исполняется ровно одна ветвь) в MVP-4.1,
пометив открытым вопрос «метрика выбора ветви».

MVP-4.1 добавил **trigger_condition** (28eaf8a6): нога в `waiting_for_trigger`
активируется, когда рыночная цена удовлетворяет условию ребра (`kConditional`).

Вопрос: как сделать OCO атомарным (одна ветвь), и нужен ли для этого новый solver-слой.

## Решение

**Атомарный one-branch OCO достигается композицией уже реализованных шагов —
trigger_condition + OCO — без нового кода.** Триггер-условие ветви *является*
метрикой выбора, которую ADR-038 считал открытой.

### Conditional OCO (атомарный)

- Ветви OCO создаются в статусе `waiting_for_trigger` (исключены из солвера).
- Каждая ветвь активируется своим `kConditional`-ребром: когда её триггер
  срабатывает (`ApplyConditionalActivations`), ветвь → `active`, и **только она**
  попадает в активный набор солвера.
- При её fill `ApplyOCOTransitions` (kOcoSibling-рёбра) отменяет остальные ветви
  (всё ещё `waiting`/`active`).

Итог: исполняется ровно одна ветвь — та, чей триггер сработал первым. «Метрика
выбора» = триггер-условие (детерминированно по рыночной цене batch). Combo
выражает это рёбрами `kConditional` (активация) + `kOcoSibling` (отмена). Runtime
уже применяет оба в child-graph шаге matching_loop.

### Unconditional OCO (остаётся eventual)

Если ветви **без триггеров** (обе безусловно активны), атомарно выбрать ветвь
**не на чем** — любой детерминированный выбор (первая нога / source) произволен и
давал бы **ложную гарантию**. Поэтому unconditional OCO остаётся
eventual/best_effort и **честно** помечается в `execution_guarantees`
(AC-F09-011, a9f368fe: «OCO eventual … best_effort»).

## Альтернативы

| Вариант | Вердикт |
| --- | --- |
| Pre-solve произвольный выбор (первая/source нога) для unconditional OCO | отклонено — произвол = ложная атомарность, нечестно |
| Market-relative выбор (лучшая цена к mid) | отложено — нужна определённая метрика «лучшей» ветви + надёжные цены (на dev price=0) |
| Новый solver-слой для one-branch | отклонено — избыточно; композиция conditional+OCO уже даёт атомарность |
| **Conditional-ветви + OCO-отмена (выбрано)** | принято — триггер = метрика выбора; без нового кода |

## Последствия

- Атомарный one-branch OCO доступен **сейчас** для conditional-ветвей (значимый
  случай — «исполни B если цена ≥ X, иначе C», взаимно отменяющие).
- Unconditional OCO — eventual best_effort, честно отражён в гарантиях.
- Новых контрактов/схем/слоёв нет. Combo для атомарного OCO несёт оба типа рёбер.
- MVP-4.1 «atomic one-branch OCO» закрыт как **design-решение** (verified
  композиционным domain-тестом), а не отдельной реализацией.

## Обратимость

Высокая. Решение — это композиция аддитивных шагов child-graph; убрать ничего не
нужно. Если в будущем понадобится market-relative выбор для unconditional OCO —
это отдельный pre-solve шаг, изолированно (как отмечено в альтернативах).
