---
id: ADR-032
status: accepted
date: 2026-06-05
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - docs/03-architecture/adr/ADR-021-floworder-lifecycle.md
  - docs/03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md
  - docs/07-data/oltp-schema.md
  - contracts/proto/fob/orders/v1/combo.proto
  - incoming-docs/IN-011.meta.md
  - CLAUDE.md (§3.3 ADR, §8.2 FlowOrder, §10.3 service boundaries)
source: IN-011 (F-09 v2 corrected §4, §8, §11.1, §11.6, §15)
---

# ADR-032: Parent/child модель пакетных и комбо-заявок (F-09)

## Контекст

Текущая модель данных знает только плоскую `flow_orders` + заготовку
`flow_order_legs(order_id, instrument_symbol, weight)`
([infra/postgres/init.sql](../../../infra/postgres/init.sql)). F-09 v2 (IN-011
§4, §8) вводит иерархию торговых намерений: один parent intent, внутри которого
ноги связаны общими условиями и графом активации (OCO/bracket/conditional).
Conflict CN-IN011-02 требует решения о модели parent/child.

## Решение

Ввести **трёхуровневую модель** parent → child → leg и отдельный граф
зависимостей.

### Сущности

- **`BatchOrder`** — клиентский parent object, объединяющий несколько дочерних
  заявок (`ComboOrder`, conditional branches, `FlowOrder`). Не путать с `Batch`
  (один цикл клиринга F-04).
- **`ComboOrder`** — многоногая заявка с типом (`pair, basket, spread,
  conditional, oco, bracket`), общим `executionMode`, `atomicityPolicy`,
  `atomicityScope`, `fallbackPolicy`, `minExecutionScale`,
  `maxRatioDeviationBps`.
- **`Leg`** — одна нога: инструмент, side, ratio/weight, price band, max
  qty/rate, filledCum, venuePreferences, status.
- **`MultiLegConstraint`** — общее ограничение группы (ratio, spread, budget,
  factor, margin, risk) с коэффициентами и severity (`hard`/`soft`).
- **`ConditionalLink`** — рёбра графа активации/отмены (OCO siblings, bracket
  entry→TP/SL, conditional).

### Persistence (OLTP, [07-data/oltp-schema.md](../../07-data/oltp-schema.md))

`batch_orders`, `combo_orders`, `combo_order_legs`, `combo_constraints`,
`conditional_links`, `execution_groups`, `group_state_transitions`.

`combo_order_legs` **расширяет** существующую `flow_order_legs` (добавляет side,
p_low/p_high, q_rate, q_max, filled_cum, status, venue_preferences). Окончательный
выбор «расширить vs новая таблица» фиксируется при реализации (затрагивает
F-02/F-04) — см. open question в [IN-011.fragment-map.md](../../../incoming-docs/IN-011.fragment-map.md).

### Граф состояний и идемпотентность

Переходы child graph (activation, OCO cancel-siblings, bracket resize-exits
\(Q_{tp}=Q_{sl}=Q_{entry}^{filled}\)) **обязаны быть идемпотентными**; журнал
переходов — `group_state_transitions` (аудит + дедуп повторных событий,
CLAUDE.md §13 idempotent consumer, ADR-020).

### Границы сервисов

- `order_flow` владеет parent/legs/constraints/graph и их нормализацией
  (Provider-Client Backend); публикует grouped `orders.normalized`.
- `matching` владеет `execution_groups`/leg fills и переходами графа в цикле
  клиринга.
- Leg нормализуется к CSLO-параметрам FlowOrder (совместимость с F-02/F-04),
  но **не** является самостоятельной FlowOrder в режиме `multileg_vector_solver`
  ([ADR-031](ADR-031-multileg-execution-modes-atomicity.md)).

## Альтернативы

- **Плоская `flow_orders` + `parent_id` колонка** — отклонено: не выражает
  constraints и граф OCO/bracket, ломает grouped reporting и атомарность.
- **Только `ComboOrder` без `BatchOrder`** — отклонено: теряется контейнер для
  смешанных групп (combo + conditional + одиночные FlowOrder) и групповая
  отмена.

## Последствия

- **Плюс:** единый parent intent с отчётностью/отменой/статусами; явная модель
  constraints и графа; аудит переходов.
- **Минус:** 7 новых таблиц + миграция; усложнение order lifecycle (новые
  статусы parent/leg/group, IN-011 §15) — согласовать с
  [ADR-021](ADR-021-floworder-lifecycle.md).

## Обратимость

Средняя. Схема таблиц обратима миграциями до GA; форма parent/child в proto и
`orders.normalized` — низкая обратимость (нужен ADR на breaking change).
