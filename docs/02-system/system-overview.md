---
id: DOC-SYS-OVERVIEW
phase: 02-system
status: draft
owner: core-team
related:
  - docs/03-architecture/architecture-overview.md
  - docs/02-system/features.md
---

# System Overview

## Назначение

Описать, что делает система Continuous Exchange как чёрный ящик.

## Внешние границы

- **Клиенты** (UI, API) подают FlowOrders и получают отчёты.
- **External Venues** (CEX/DEX) поставляют ликвидность и принимают хедж-ордера.
- **Custody / Blockchain** обеспечивает деньги.
- **Operators / Regulators** управляют системой и получают отчётность.

См. [../03-architecture/context-diagram.md](../03-architecture/context-diagram.md).

## Основные функциональные блоки

1. **Onboarding и auth** — F-01.
2. **Управление flow orders** — F-02 (создание), F-03 (изменение/отмена).
3. **Непрерывный клиринг** — F-04, F-09, F-10.
4. **Рыночные данные** — F-05, F-11.
5. **Учёт и риск** — F-06, F-07, F-08.
6. **Хедж и сеттлмент** — F-12, F-14.
7. **Отчётность, replay, мониторинг** — F-13, F-15, F-17.
8. **Операционные функции** — F-16.

См. полный каталог в [features.md](features.md).

## Качественные характеристики

- median solveTimeMs ≤ 500 ms, p95 ≤ 1000 ms.
- Детерминированность исполнения (replay даёт идентичный BatchResult).
- At-least-once семантика для Kafka.
- ACID для PostgreSQL (планируется).

См. [non-functional-requirements.md](non-functional-requirements.md).

## Продуктовая модель (из IN-001 §3)

CES реализует **непрерывный во времени** механизм, отличающийся от классических CLOB/AMM:

- Сделка представляется как **поток объёма во времени** с конечной скоростью \(v_t\), а не как дискретный обмен в момент.
- В каждый момент существует **равновесная цена и скорость**, определяемые пересечением совокупного спроса и предложения в пространстве «цена–скорость».
- Участники задают предпочтения через **CSLO-кривые** (continuous scaled limit orders) или эквивалентные потоковые заявки.

### Типы операций

1. **Обычные одно-активные заявки** — мгновенные («market-like») или растянутые во времени (TWAP-подобные).
2. **Потоковые заявки** — `FlowOrder` с диапазоном цены, скоростью, окном.
3. **Портфельные / парные** — корзина активов с весами; согласованное исполнение.
4. **MM-кривые** — двусторонние CSLO для market maker'ов.

### Модель цены и воздействия

- **Fundamental price** \(P_t\) — модельируемая «справедливая» цена.
- **Temporary impact** — пропорционален скорости \(v\), исчезает после завершения сделки.
- **Permanent impact** — пропорционален объёму, остаётся как inventory shift у MM.

Подробности — в [../04-domain/domain-overview.md](../04-domain/domain-overview.md) и [glossary §IN-001 §1–9](../01-business/glossary.md#дополнения-из-in-001-19).

## Связь с остальной документацией

- Архитектура реализации → [03-architecture/](../03-architecture/)
- Контекст (C1) → [../03-architecture/context-diagram.md](../03-architecture/context-diagram.md)
- Контейнеры (C2) → [../03-architecture/container-diagram.md](../03-architecture/container-diagram.md)
- Домен → [04-domain/](../04-domain/)
- Контракты → [06-api/](../06-api/)
- Функциональные требования → [functional-requirements.md](functional-requirements.md)
- Нефункциональные → [non-functional-requirements.md](non-functional-requirements.md)
- Актёры → [actors.md](actors.md)

## Source Fragments

- IN-001-FR-013 — §3 обзор продуктовой модели
