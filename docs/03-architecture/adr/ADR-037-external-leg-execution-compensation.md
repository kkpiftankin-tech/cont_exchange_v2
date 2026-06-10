---
id: ADR-037
status: accepted
date: 2026-06-10
owners:
  - architecture
  - core-team
related:
  - docs/03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md
  - docs/03-architecture/adr/ADR-035-oco-bracket-runtime.md
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - docs/02-system/features/F-12-execution-hedge/
  - cpp/matching/src/app/execution_intent_builder.hpp
  - cpp/matching/src/infra/kafka/execution_intents_producer.hpp
  - cpp/venues/src/infra/execution_intents_consumer.hpp
  - contracts/proto/fob/execution/v1/execution.proto
source: IN-011 (F-09 v2) §11.7, §12.10; ADR-031 (external_compensating scope)
---

# ADR-037: External-leg execution + compensation для combo (MVP-5)

## Контекст

ADR-031 ввёл `atomicity_scope = external_compensating`: combo может иметь ноги,
исполняемые на **внешних venue** (CEX/DEX), не во внутреннем matching. Сейчас
такие combo принимаются (policy `external_compensating_enabled`, validate
запрещает лишь `strict_atomic`+external), но external-ноги **никуда не
маршрутизируются** — grouped solver обрабатывает все ноги как внутренние.

Инфраструктура для внешнего исполнения **уже есть** (F-12 hedge):
`ExecutionIntent` → `execution.intents` (producer в matching:
`execution_intent_builder` + `execution_intents_producer`) → `venues`
(`execution_intents_consumer` исполняет) → `ExecutionReport`
(`execution.venue`/`execution.reports`) → `ledger` постит. Risk имеет
`PreHedgeCheck`.

Нерешённое: (1) как combo выражает external-ногу и кто её маршрутизирует;
(2) что значит «compensating» при сбое внешней ноги.

## Решение

### 1. Идентификация external-ног

Нога — **external**, если её `venue_preferences` непусто и не содержит
`"internal"` (пусто / `"internal"` → внутренняя). Маркер вычисляется при загрузке
в matching.

### 2. Маршрутизация (переиспользуем F-12 путь)

В grouped-batch matching **разделяет ноги активной группы**:
- **internal** → существующий grouped solver (вектор `e`, ExecutionGroup) — без
  изменений;
- **external** → `ExecutionIntent` **строится напрямую** из ноги и публикуется
  через существующий `execution_intents_producer` → `execution.intents`
  (`internal_order_id = leg_id`, `instrument`, `side` из знака `target_ratio`,
  `target_qty = q_max − filled_cum`, `allowed_venues = venue_preferences`). venues
  исполняет, ledger постит report (как F-12).

> **Правка по факту реализации:** `execution_intent_builder` (app) — это
> hedge-планировщик F-12 (OrderForecast / hedge_trigger_policy / double-цены), не
> переиспользуется для combo-ног. Строим generic `ExecutionIntent` (proto) прямо.
> `VectorLeg` (matching domain) расширяется `venue_preferences` + флагом external;
> loader их грузит, grouped-batch исключает external из solve-вектора.

External-ноги **не входят** во внутренний solve-вектор (как cancelled/waiting уже
исключены). Так internal и external исполняются независимо — что и есть смысл
`external_compensating` (не атомарно).

### 3. Compensation при сбое внешней ноги

`external_compensating` = **внутренние ноги уже исполнены, внешняя может не
исполниться**. При `ExecutionReport.status ∈ {REJECTED, TIMEOUT, CANCELLED}` для
combo-external-ноги, когда у combo уже есть исполнение внутренних ног, фиксируется
**требование компенсации**:

- новая таблица `combo_compensations (compensation_id, parent_order_id, leg_id,
  reason, internal_filled_qty, status ∈ {pending, resolved, cancelled},
  created_at)`; идемпотентно по `(parent_order_id, leg_id, report_id)`;
- consumer (ledger или новый) на `execution.venue`: для combo-ног с провалом →
  `INSERT … pending`.

**Сам компенсирующий трейд (реверс internal / повторный хедж) — НЕ в MVP-5.** Он
policy/operator-driven (риск каскадных авто-реверсов над деньгами) → MVP-6.
MVP-5 делает провал **видимым и учтённым**, а не молча проглоченным.

### 4. Честность (AC-F09-011)

`external_compensating` combo уже честно помечен «не атомарно; возможна
компенсирующая транзакция» (a9f368fe). MVP-5 это реализует фактически.

### MVP-5 scope (срез)

1. matching: split internal/external + emit `ExecutionIntent` для external-ног
   (reuse builder/producer). [slice 1]
2. data: `combo_compensations` DDL + repo. [slice 2]
3. compensation consumer: `execution.venue` proval combo-ноги → `pending`. [slice 3]
4. Линковка report→combo-leg (по `internal_order_id = leg_id`).

**Deferred (MVP-6):** авто-реверс/повторный хедж, partial-fill compensation math,
cross-venue routing/best-execution, hedge PnL атрибуция на combo.

## Альтернативы

| Вопрос | Вариант | Вердикт |
| --- | --- | --- |
| Маршрутизация | order_flow эмитит intent при create | отклонено — нет batch-контекста/цен; matching уже владеет intent-эмиссией (F-12) |
| Маршрутизация | **matching split + reuse F-12 (выбрано)** | принято |
| Compensation | авто-реверс internal сразу | отклонено в MVP — каскадный риск над деньгами без operator-контроля |
| Compensation | **marker `combo_compensations` pending (выбрано)** | принято — видимо/учтено, реверс в MVP-6 |
| External-маркер | новое поле в proto/DDL | отклонено — `venue_preferences` уже несёт это |

## Последствия

- External combo-ноги реально исполняются (reuse F-12), без нового venue-слоя.
- Сбой внешней ноги фиксируется (`combo_compensations`), не теряется.
- Internal/external не атомарны — соответствует honest-mode.
- Новое: `combo_compensations` (OLTP), compensation consumer, split в grouped batch.
- F-12 hedge-путь не ломается (combo-external и hedge различаются по
  `internal_order_id` namespace + отсутствию `hedge_flow_id`).

## Обратимость

Высокая. Split аддитивен и gated на наличие external-ног; combo без них —
без изменений. `combo_compensations` — отдельная таблица (drop безопасен).
Авто-реверс (MVP-6) — отдельный шаг, изолированно.
