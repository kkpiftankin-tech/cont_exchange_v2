# F-09 — Batch, Combo and Multi-leg Orders

> **Статус:** `in-progress` (PR #13). **MVP-1 + MVP-2 завершены.**
> MVP-2 (`scalable_atomic` ratio/basket) работает **live end-to-end**: order_flow
> (policy gate 002 + risk gate 040) → matching (grouped solver 043–048) →
> `execution.groups` (Kafka+PG) → ledger grouped postings (060). proto enrichment
> (062), GAP `filled_cum` закрыт, полный E2E (090) verified live. Дальше — MVP-3+
> (spread/factor QP, OCO/bracket, external legs, frontend/observability).
> Источник: [incoming-docs/2026-06-05-F-09-batch-combo-orders-v2.md](../../../../incoming-docs/2026-06-05-F-09-batch-combo-orders-v2.md) (IN-011).

## Описание

F-09 вводит заявки, где торговый intent относится не к одному инструменту, а к
**связанной группе ног**: `BatchOrder`, `ComboOrder`, `pair`, `basket`,
`spread`, `OCO`, `bracket`, а также factor-/budget-/risk-constrained заявки.
Группа исполняется как **единый бизнес-объект** с общей логикой жизненного
цикла, pre-trade риска, клиринга, исполнения, отчётности, статусов и replay.

## Ключевое архитектурное решение — два режима

| Режим | Что гарантирует | ADR |
| --- | --- | --- |
| `orchestration_only` | Удобный parent object для группы независимых ног. **Не** сохраняет weights/ratio/spread/portfolio exposure. Только при явном выборе + предупреждение. | [ADR-031](../../../03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md) |
| `multileg_vector_solver` | Настоящее многоногое исполнение: matching выбирает **согласованный вектор** \(e_g\); результат — один `ExecutionGroup`. **Обязателен** для weights/ratio/spread/factor/budget/leverage/margin/portfolio-risk. | [ADR-031](../../../03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md) |

**Правило (IN-011 §21):** нельзя реализовать «настоящую F-09» через набор
независимых `FlowOrder` с общим `parentId` — это допустимо только как
`orchestration_only` и не обещает multi-leg guarantees.

## Политики атомарности

`strict_atomic` · `scalable_atomic` · `best_effort` · `sequential_fallback` ·
`external_compensating`. Область — `atomicityScope`: `internal_batch` /
`venue_native` / `external_compensating` / `none`. Подробности и инварианты —
[business-rules.md §F-09](../../../04-domain/business-rules.md).

## Таксономия типов заявок (единая модель)

Все типы — частные спецификации одной модели `MultiLegVectorOrder + Legs +
Constraints + StateGraph + ExecutionPolicy`:

| Тип | Что фиксируем | comboType |
| --- | --- | --- |
| Basket | веса \(w_i\) | `basket` |
| Pair | ratio \(Q_1 = kQ_2\) | `pair` |
| Spread | \(L \le c^T P \le U\) | `spread` |
| Factor | \(F Q = 0\) (нейтральность) | `basket` + `factor_neutrality` |
| Budget | \(P^T Q \le N\) | `basket` + `max_total_notional` |
| Risk | \(R(Q) \le R_{max}\) | `*` + risk constraints |
| OCO | граф переходов состояний | `oco` |
| Bracket | граф + зависимость объёмов \(Q_{tp}=Q_{sl}=Q_{entry}^{filled}\) | `bracket` |

## User Stories (IN-011 §6)

- US-F09-001 Pair order (buy BTC / sell ETH в заданном ratio).
- US-F09-002 Basket order (целевые веса портфеля).
- US-F09-003 Spread order (исполнение только при допустимой цене комбинации).
- US-F09-004 OCO (исполнение одной ветви отменяет другую).
- US-F09-005 Bracket (entry + TP + SL на фактический объём входа).
- US-F09-006 Контролируемый fallback (видеть degraded/compensating).

## Use Cases

- [UC-F09-01 — Создание combo/multi-leg заявки](../../use-cases/UC-F09-01-create-combo-order/use-case.md)
- [UC-F09-02 — Grouped matching внутри batch cycle](../../use-cases/UC-F09-02-grouped-matching/use-case.md)
- [UC-F09-03 — External leg execution / compensating](../../use-cases/UC-F09-03-external-leg-execution/use-case.md)

## Frontend Scope

Редактор parent/legs/constraints; grouped preview (expected scale, leg fills,
combined VWAP/IS, ratio/spread deviation, binding leg/constraints); отображение
group/leg status и fallback/degraded/rollback состояний (IN-011 §12.1).

## Контракты и данные

- gRPC: [order-flow-create-combo-order.md](../../../06-api/grpc/order-flow-create-combo-order.md)
- Kafka: [execution-groups.md](../../../06-api/messaging/execution-groups.md) ([topics.md](../../../06-api/messaging/topics.md))
- OLTP/OLAP: [oltp-schema.md](../../../07-data/oltp-schema.md) (combo_*), [olap-schema.md](../../../07-data/olap-schema.md) (grouped_*)
- Домен: [entities.md](../../../04-domain/entities.md), [business-rules.md §F-09](../../../04-domain/business-rules.md)

## Acceptance Criteria

См. [acceptance-criteria.md](acceptance-criteria.md) (AC-F09-001..010) и
[feature.yaml](feature.yaml).

## Out of scope (MVP)

Cross-venue true atomic без native support, options solver, нелинейные Greeks,
RL-routing, полный rollback внешних сделок, prime-broker netting.

## Known Gaps (MVP-2)

Зафиксированный техдолг grouped-исполнения (на момент MVP-2, PR #13):

1. ✅ **РЕШЕНО** (коммиты e9f9b8bf, fc253dd0). Partial-группы накапливают
   `filled_cum` между batch-циклами: `PostgresExecutionGroupsRepository`
   применяет `LegResult.exec_qty` к `combo_order_legs.filled_cum`
   (`LEAST(... , q_max)`), идемпотентно по `execution_group_id` (gate на
   `affected_rows`). Combo → `filled`, когда **любая** нога достигла `q_max`
   (для ratio-locked группы это её максимум). `matching_loop` не публикует
   пустые ExecutionGroup (no-op batch). Проверено live: basket BTC(0.6)/ETH(0.4),
   q_rate=3 сходится за 4 batch'а (соотношение сохранено) → combo filled.
2. **`exec_price`** берётся из market_data reference prices; для не котируемых
   на стенде символов = 0 (qty/scale считаются корректно, notional = 0).
3. **Risk:** групповой pre-trade в `CreateComboOrderUseCase` — заглушка-approve;
   реальный `RiskService.PreTradeCheckGroup` — T-F09-040.
4. **Downstream:** ledger пока не потребляет `execution.groups` (T-F09-060) —
   баланс по grouped fills не применяется до реализации.

## Implementation

**MVP-1 (`orchestration_only`) + MVP-2 grouped solver реализованы (PR #13).**
MVP-2 grouped-исполнение работает live end-to-end до `execution_groups`
(PG + Kafka). Прогресс и оставшиеся задачи (060 ledger, 040 risk, 090 E2E):
[implementation-plan/F-09-batch-combo-orders.tasks.md](../../../implementation-plan/F-09-batch-combo-orders.tasks.md).

## Source Fragments

- IN-011 (полностью; карта — [IN-011.fragment-map.md](../../../../incoming-docs/IN-011.fragment-map.md))
- Historical: IN-001-FR-027, IN-001-FR-028 (исходная упрощённая модель, переформулирована ADR-031/032).
