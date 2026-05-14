# F-04 — Batch Clearing Cycle

> **Статус:** in-progress, MVP-симулятор. Реальный солвер D(p)=0 не реализован.

## Описание

Периодический батч-клиринг активных FlowOrder. Каждые `batchIntervalMs` мс solver вычисляет клиринговые цены и скорости исполнения, формирует `BatchResult` и набор `FillEvent`, публикует в Kafka.

## Ключевые сущности

- **Batch** — один шаг клиринга.
- **BatchResult** — сводка батча. Proto: [`fob.matching.v1.BatchResult`](../../../../contracts/proto/fob/matching/v1/batch.proto).
- **FillEvent** — конкретное исполнение конкретного FlowOrder.
- **clearPrices** — вектор равновесных цен по инструментам.
- **executedRates** — итоговые скорости исполнения.
- **residualNorm** — мера невязки спроса/предложения. Меньше — лучше.
- **solveTimeMs** — время работы solver'а (для SLA).

## Реализация

- [cpp/matching/src/app/matching_loop.cpp](../../../../cpp/matching/src/app/matching_loop.cpp)
- [cpp/matching/src/domain/solver.hpp](../../../../cpp/matching/src/domain/solver.hpp) — placeholder интерфейс для будущего LP/QP

## Acceptance criteria

См. [acceptance-criteria.md](acceptance-criteria.md).

## Известные несоответствия спецификации

См. [feature.yaml](feature.yaml) → `knownIssues`. Кратко:

### Критические

1. **Утечка резерва при BUY** — после полного fill в reserved висит разница `(price_high - midpoint) * qty`.
2. **Двойной учёт при cancel после partial fill** — ledger снимает оригинальную сумму резерва, а не остаток.

### Архитектурные gap'ы

3. **Нет настоящего солвера** — каждый ордер обрабатывается независимо.
4. **Цена fill — per-order midpoint**, а не единая клиринговая на инструмент.
5. **`residualNorm = 0.0` захардкожен.**
6. **`solveTimeMs = 1` захардкожен.**
7. **`executed_rates` показывает `max_speed`**, а не фактическую скорость.
8. **Источник данных — Kafka**, а не PostgreSQL `floworders`.
9. **Нет интеграции с Market Data** за reference prices.
10. **Один Kafka топик `batch.outputs` вместо двух (нет `fills`).**
11. **Отсутствуют поля `liquidity_source`, `fees`, `fill_id`** в Fill.
12. **Нет fallback** при non-convergence.
13. **Нет SLA-метрик и алертов.**
14. **Нет multi-leg / portfolio orders.**

## Связанные фичи

- F-02 (Create FlowOrder) — поставщик `orders.normalized`
- F-06 (Positions / PnL / Margin) — потребитель `batch.outputs`
- F-07 (Pre-trade Risk) — pre-trade гейт
- F-09 (Batch / Combo Orders) — расширение F-04 на multi-leg
- F-11 (External Venues LOB → FOB) — внешняя ликвидность
- F-12 (Execution Hedge) — после внутреннего клиринга

## Traceability

См. [traceability.yaml](traceability.yaml).

## Acceptance Criteria (IN-001)

Система должна регулярно (каждые `batch_interval_ms`) находить равновесную цену и скорости исполнения, распределять объём по всем активным заявкам и фиксировать факты сделок (`FillEvent`).

- `BatchResult` сохраняет diagnostics: `residual_norm`, `solve_time_ms`, `num_active_orders`, `config_version`.
- Replay с тем же входом и `config_version` даёт идентичный результат (F-15).
- Fills атомарно применяются к Ledger по `batch_id + order_id` (идемпотентно).
- Solver SLA: p50 ≤ 200 ms, p95 ≤ 500 ms.

Источник: IN-001 §6 FR-CLEAR-001/002, §7 NFR-EXEC-002.

## Detailed spec (IN-003)

Полное описание — в архиве [`incoming-docs/2026-05-13-F-04-Batch-Clearing-v1.md`](../../../../incoming-docs/2026-05-13-F-04-Batch-Clearing-v1.md). Ключевые акценты ниже.

### Бизнес-цели F-04

- Честное и детерминированное исполнение FlowOrder в батчах с учётом их лимитов `p_L`, `p_H`, `q_rate`, `Q_max`.
- SLA по solve time: median ≤ 500 ms, p95 ≤ 1000 ms (MVP); подробная разбивка по размеру батча — в [non-functional-requirements §NFR-EXEC-002](../../non-functional-requirements.md).

### Альтернативные сценарии

1. **Нет активных заявок** — пустой `BatchResult` (без FillEvent); метрика «пустой батч» в Observability.
2. **Солвер не сошёлся** (`residual_norm > tolerance` или `max_iterations`) — `BatchResult` с флагом `degraded`; `risk.alerts(SOLVER_NOT_CONVERGED)`; опциональный fallback на epsilon-MM или остановка торгов.
3. **Нарушен SLA `solve_time_ms`** — `BatchResult` с признаком `sla_breach`; alert; возможный kill-switch (F-16).

Подробные диаграммы — в [UC-F04-01 use-case Alternative Flows](../../use-cases/UC-F04-01-run-batch-clearing/use-case.md#alternative-flows).

### UX / UI

Три экрана:

- **Торговля / Активные заявки** — FlowOrders с `filled_cum`, лимиты `p_L`, `p_H`, `q_rate`, `Q_max`, фильтры, действия.
- **Исполнения / История клиринга** — список FillEvent с ценой, объёмом, `batch_id`, `liquidity_source` (internal / cex_hedge / dex_hedge / epsilon_mm).
- **Диагностика клиринга** — для операторов: `clear_prices`, `executed_rates`, `residual_norm`, `solve_time_ms` по батчам, drill-down в FillEvent.

### Definition of Done

См. [implementation-plan/F-04-batch-clearing.tasks.md → DoD](../../../implementation-plan/F-04-batch-clearing.tasks.md#definition-of-done-in-003).

### Тест-план

См. [10-testing/features/F-04-test-plan.md](../../../10-testing/features/F-04-test-plan.md) — юнит-тесты U1–U10, SLA-таблица по размеру батча, ручные сценарии.

### Внутренняя последовательность

См. [SEQ-MATCHING-001-solver-cycle](../../../05-components/matching-fob-core/sequences/SEQ-MATCHING-001-solver-cycle.md) — internal cycle (Scheduler → RunBatch → loadActiveFlowOrders → getReferencePrices → solveBatch → publish).

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
- IN-003-FR-006 (deterministic execution)
- IN-003-FR-007 (область применения, stakeholders)
- IN-003-FR-008 (цели F-04)
- IN-003-FR-011 (alternative flows)
- IN-003-FR-012 (UX/UI)
- IN-003-FR-013 (критерии успеха)
