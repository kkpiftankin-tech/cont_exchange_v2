# F-09 — Batch and Combo Orders

> **Статус:** Not implemented. Заглушка для traceability и планирования.

## Описание

Multi-leg FlowOrder: одна заявка задаётся не одним инструментом, а корзиной с весами `w_i` по активам. Использование: спред-торговля, портфельные ребалансировки, корзинная ликвидация.

## Что нужно сделать

1. Расширить proto `FlowOrder` полем `repeated OrderLeg legs = N;` где `OrderLeg { string asset; Decimal weight; }`.
2. matching solver должен поддержать многомерные ограничения по корзине.
3. ledger ApplyBatchResult должен корректно применять mirror legs.
4. risk должен оценивать корреляции внутри корзины.

Подробности — в [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна поддерживать заявки, затрагивающие несколько активов с заданными весами (корзины, спреды, парные), и исполнять их согласованно (all-or-nothing на уровне batch).

- `portfolio_weights` в `flow_orders` определяет legs.
- Risk Manager проверяет совокупный exposure портфеля.
- Ledger резервирует net-position по quote-валюте.
- Все legs закрываются одним `combo_id` для трассировки.

Источник: IN-001 §6 FR-ORDER-001 (тип 3), §5.1 пример портфельной заявки.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
