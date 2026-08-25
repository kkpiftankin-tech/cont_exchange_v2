---
id: ADR-039
status: accepted
date: 2026-06-10
owners:
  - architecture
  - core-team
  - risk
related:
  - docs/03-architecture/adr/ADR-037-external-leg-execution-compensation.md
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - docs/02-system/features/F-16-operator-console/
  - cpp/matching/src/infra/postgres_combo_compensation_repository.cpp
  - cpp/risk/src/app/risk_uc.cpp
  - infra/postgres/init.sql
  - CLAUDE.md (§9 money, §17 ledger, §22 operator audit)
source: ADR-037 §3 (deferred MVP-6); F-09 v2 §11.7
---

# ADR-039: Compensation resolution для combo (MVP-6, operator-driven)

## Контекст

ADR-037 (MVP-5) фиксирует провал внешней combo-ноги как
`combo_compensations(pending)`, но **сам компенсирующий трейд отложил в MVP-6**:
при `external_compensating` внутренние ноги уже исполнены, внешняя — нет, и combo
осталась несбалансированной (например: BTC куплен внутри, ETH не продан на venue →
незахеджированная экспозиция).

Авто-реверс денег — **самая рискованная зона** (CLAUDE.md §9 money, §17 ledger:
«запрещено settlement на floating-point», «audit trail»). Автоматический каскадный
реверс без operator-контроля может усугубить потери (реверс по плохой цене, петля
реверсов). Поэтому нужно проектное решение.

## Решение

### 1. Operator-driven, НЕ авто-каскад (MVP-6)

Resolution компенсации — **operator-action** (как kill-switch, ADR/§22): оператор
видит pending-компенсацию (F-16 console) и **явно** выбирает действие. Авто-policy
(система сама реверсит по правилам) — **MVP-7**, после накопления статистики и
risk-review. Это снимает каскадный риск.

### 2. Действия resolution

| Действие | Что делает |
| --- | --- |
| `reverse_internal` | создаёт **реверсивную FlowOrder** (противоположная сторона, объём = фактический внутренний fill combo) через order_flow — разворачивает внутреннюю экспозицию |
| `retry_external` | повторно эмитит `ExecutionIntent` для внешней ноги (другая venue / позже) |
| `accept` | оператор принимает экспозицию как есть (dismiss) |

### 3. Внутренняя экспозиция считается на момент resolution

`combo_compensations.internal_filled_qty` (записан в MVP-5) — лишь информационный
маркер (это был filled внешнего report'а). Реальный объём для `reverse_internal`
**пересчитывается из `combo_order_legs`** (сумма `filled_cum` внутренних ног combo)
на момент resolution — источник истины, а не устаревший снимок.

### 4. Статус, аудит, идемпотентность

`combo_compensations` расширяется: `resolution_action TEXT`, `operator_id TEXT`,
`resolving_ref TEXT` (id реверсивной FlowOrder / retry-intent), `resolved_at`.
Переход `pending → resolved | cancelled` **идемпотентен** (gate на `status =
'pending'`; повторный resolve — no-op). Каждое действие — в audit (risk.alerts /
operator audit log) с `operator_id` (§22).

### 5. Авторизация + транспорт

gRPC `ResolveCompensation(compensation_id, action, operator_id)` —
**operator-authorized** (как `SetKillSwitch`). Размещение: order_flow (владеет
созданием FlowOrder для `reverse_internal`) либо operator-control (F-16); решение —
order_flow с operator-auth guard. Деньги/реверс идут через обычный order_flow →
matching → ledger pipeline (никакого прямого мутирования ledger).

### MVP-6 scope (срез)

1. DDL: `combo_compensations` += resolution-колонки. [slice 1]
2. repo: `ResolvePending(compensation_id, action, operator_id, resolving_ref)`
   (идемпотентно) + `ListPending()`. [slice 2]
3. gRPC `ResolveCompensation` (operator-auth) → для `reverse_internal` считает
   внутренний fill из `combo_order_legs`, создаёт реверсивную FlowOrder, ставит
   `resolved`. [slice 3]
4. F-16 console: список pending + кнопки действий (frontend-spec). [slice 4]

**Deferred (MVP-7):** авто-policy resolution; partial-fill математика реверса;
cross-venue retry-роутинг; атрибуция hedge-PnL компенсации.

## Альтернативы

| Вопрос | Вариант | Вердикт |
| --- | --- | --- |
| Триггер | авто-реверс по правилу сразу | отклонено в MVP — каскадный money-риск без operator-контроля (§9/§17) |
| Триггер | **operator-driven (выбрано)** | принято — оператор решает, авто — MVP-7 |
| Реверс | прямое мутирование ledger | отклонено — нарушает §17; реверс через order_flow pipeline |
| Объём реверса | сохранённый `internal_filled_qty` | отклонено — устаревший маркер; считаем из `combo_order_legs` |
| Размещение | risk (как kill-switch) | отложено — реверс создаёт FlowOrder → order_flow ближе |

## Последствия

- Провалы combo больше не «висят» — оператор их разрешает с аудитом.
- Реверс идёт штатным pipeline (order_flow→matching→ledger) — money-инварианты целы.
- Новое: resolution-колонки, repo-методы, operator gRPC, F-16 UI.
- Авто-policy + partial-математика — MVP-7 (после risk-review).

## Обратимость

Высокая. Resolution-слой аддитивен поверх MVP-5 marker'а; колонки nullable;
operator gRPC изолирован. Переход на авто-policy (MVP-7) — отдельный слой над тем
же repo, не ломает operator-путь.
