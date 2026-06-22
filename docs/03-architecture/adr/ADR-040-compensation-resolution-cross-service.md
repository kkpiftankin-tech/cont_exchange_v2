---
id: ADR-040
status: accepted
date: 2026-06-11
owners:
  - architecture
  - core-team
related:
  - docs/03-architecture/adr/ADR-039-compensation-resolution.md
  - docs/03-architecture/adr/ADR-037-external-leg-execution-compensation.md
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - cpp/matching/src/infra/postgres_combo_compensation_repository.cpp
  - cpp/order_flow/src/transport/grpc_order_flow_service.cpp
  - contracts/proto/fob/matching/v1/compensation.proto
  - CLAUDE.md (§10.3 service boundaries, §14/§17 single table owner, §22 operator audit)
source: ADR-039 §5 (placement open question); F-09 MVP-6 slice 3b
---

# ADR-040: Compensation resolution — размещение endpoint и cross-service доступ (MVP-6 slice 3b)

## Контекст

ADR-039 (§5) решил, что operator-action `ResolveCompensation` для `reverse_internal`
идёт через **order_flow** (он владеет созданием FlowOrder), но оставил **открытым
механизм доступа** к таблице `combo_compensations`: она принадлежит **matching**
(`PostgresComboCompensationRepository` пишет `pending` в MVP-5, имеет `ListPending`
/ `ResolvePending`). Возникает конфликт двух правил CLAUDE.md:

- §10.3 — matching не создаёт/не резервирует ордера (создание reversing-FlowOrder
  обязано идти через order_flow);
- §14/§17 — у таблицы один сервис-владелец (нельзя, чтобы и matching, и order_flow
  писали `combo_compensations` напрямую).

Нужно зафиксировать, где живёт operator-endpoint и как order_flow читает/резолвит
компенсацию, не нарушая ни одно правило.

## Решение

### 1. Endpoint — в order_flow, владение таблицей остаётся за matching

Operator-facing gRPC `ResolveCompensation` (operator-auth, как `SetKillSwitch`)
размещается в **order_flow**: там уже combo API, operator surface и pipeline
`CreateFlowOrder`. **`combo_compensations` остаётся единолично за matching** —
order_flow не читает и не пишет её напрямую.

### 2. matching экспонирует CompensationService (gRPC)

Новый gRPC-сервис `fob.matching.v1.CompensationService` поверх существующего
репозитория (read + idempotent-resolve, без создания ордеров):

| rpc | Назначение |
| --- | --- |
| `ListPendingCompensations` | список pending для operator-console (F-16) |
| `GetPendingCompensation` | деталь по `compensation_id` (parent_order_id, leg_id, reason) |
| `ResolvePending` | идемпотентный переход `pending → resolved\|cancelled` + audit (`operator_id`, `resolving_ref`) — вызывается order_flow **после** создания reversing-ордера |

matching остаётся единственным writer'ом таблицы. Это операция управления
состоянием компенсации, не торговое решение — границы §10.3 не нарушены.

### 3. Оркестрация в order_flow

```
operator → order_flow.ResolveCompensation(compensation_id, action, operator_id)
  1. matching.GetPendingCompensation(compensation_id)         (gRPC, read)
  2. reverse_internal:
       load internal legs из combo_order_legs (own repo, filled_cum) — §3 ADR-039
       ComputeReversals(internal_legs) → ReversalOrder[]       (pure, slice 3a)
       для каждого → CreateFlowOrder(...)                       (local pipeline)
       resolving_ref = join(reversing_order_ids)
     retry_external:  resolving_ref = новый ExecutionIntent id (re-emit)
     accept:          resolving_ref = "" 
  3. matching.ResolvePending(compensation_id, action, operator_id, resolving_ref) (gRPC)
```

Источник объёма реверса — `combo_order_legs.filled_cum` (ADR-039 §3), не устаревший
`internal_filled_qty`. Деньги идут только через order_flow → matching → ledger.

### 4. ComputeReversals — переезжает в shared

Чистая функция `ComputeReversals` (slice 3a, сейчас `cpp/matching/src/domain/
compensation_reversal`) нужна order_flow. Переносится в `cpp/common`
(pure, без matching-зависимостей), matching продолжает её использовать. Альтернатива
(дублировать в order_flow/domain) отклонена — DRY для money-логики.

### 5. Идемпотентность и аудит

`ResolvePending` идемпотентен по `status='pending'` (повторный вызов — no-op), что
делает весь endpoint безопасным к ретраям: если order_flow создал reversing-ордер,
но упал до `ResolvePending`, повтор увидит ту же pending-компенсацию и (т.к.
`CreateFlowOrder` идемпотентен по `client_order_id = compensation_id+leg`) не создаст
дублей. Каждый resolve — в operator audit с `operator_id` (§22).

## Альтернативы

- **Endpoint в matching, order_flow-client в matching** — 1 hop вместо 2, но
  operator-метод в matching неестественен для F-16 console и заводит matching →
  order_flow client (matching начинает инициировать создание ордеров). Отклонено.
- **order_flow читает `combo_compensations` напрямую** — меньше кода, но два
  writer'а у одной таблицы (§14/§17). Отклонено.

## Последствия

- (+) Чистые границы: matching — владелец таблицы, order_flow — владелец создания
  ордеров и operator API; оба правила CLAUDE.md соблюдены.
- (+) Переиспользуется существующий паттерн gRPC-клиентов order_flow (`risk_client`,
  `ledger_client`).
- (−) 2 cross-service hop на resolution (приемлемо: operator-action, не hot path).
- (−) Новый gRPC-сервис на matching + новый client на order_flow + переезд
  `ComputeReversals` в common.

## Обратимость

Обратимо. `CompensationService` аддитивен (новый proto, backward-compat);
переезд `ComputeReversals` — механический рефактор; endpoint за operator-auth можно
снять флагом. Откат — `git revert` без миграций данных (DDL уже на месте, MVP-5/6.1-2).
