# UC-F04-01. Выполнить цикл batch clearing

## Feature

- [F-04. Batch Clearing](../../features/F-04-batch-clearing/)

## Primary Actor

System (Matching Backend timer)

## Supporting Actors

- Traders (косвенно — владельцы исполненных заявок)

## Preconditions

- Пользователи авторизованы (F-01), имеют счета и балансы.
- В системе есть активные FlowOrder со статусом `active`, прошедшие pre-trade проверку (F-02, F-07).
- Доступны reference prices и базовый market data (mid, bid/ask) от Market Data Service.
- Конфигурация солвера (`solver_config`) задана и активна (поля: `batch_interval_ms`, `max_iterations`, `epsilon_liquidity`, `tolerance`, `fee_model`, `is_active=TRUE`).

## Trigger

Внутренний Scheduler в Matching Backend срабатывает каждые `solver_config.batch_interval_ms` (типичное значение MVP: 100–1000 ms).

## Main Flow

1. Matching читает активные ордера.
2. Solver рассчитывает clearing prices и executed rates.
3. Matching формирует `BatchResult` с fills и order updates.
4. Matching публикует `batch.outputs`.
5. Ledger применяет fills к балансам.
6. Risk Manager обновляет позиции и проверяет post-trade лимиты.
7. Observability логирует diagnostics.

## Alternative Flows

### A1. Нет активных ордеров

1. Scheduler срабатывает; Matching читает `flow_orders WHERE status='active'`, получает пустой список.
2. Создаётся `BatchResult` с пустыми `executed_rates` и без `FillEvent`.
3. Publish в `batch.outputs` с пустым batch (для аудита/непрерывности счётчика).
4. Observability фиксирует метрику «пустой батч»; никаких алертов.

### A2. Солвер не сошёлся

Условие: `residual_norm > tolerance` или достигнут `max_iterations`.

1. `BatchResult` помечается флагом `degraded` в `solver_diagnostics` (`stopped_by_max_iterations=true`).
2. Risk Manager публикует `risk.alerts(SOLVER_NOT_CONVERGED)` с подробностями.
3. Опциональный fallback: задействовать `epsilon-MM` (epsilon_liquidity из solver_config) для сглаживания, либо приостановить ближайшие батчи до ручного решения.
4. Observability собирает дополнительную диагностику для разбора.

### A3. Нарушен SLA `solve_time_ms`

Условие: `solve_time_ms > SLA-предел` для текущего размера батча.

1. `BatchResult` помечается признаком `sla_breach=true`.
2. Observability поднимает alert с порогом p95 за последние 5 минут (см. NFR-EXEC-002).
3. Возможный kill-switch (F-16) при систематическом нарушении.

## Postconditions

- Опубликовано событие `batch.outputs`.
- Применены fills в Ledger.
- Обновлены позиции в Risk Manager.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F04-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F04-UC-F04-01-services.md)
- [Internal matching-fob-core sequence (SEQ-MATCHING-001-solver-cycle)](../../../05-components/matching-fob-core/sequences/SEQ-MATCHING-001-solver-cycle.md)

## Related Contracts

- [batch.outputs](../../../06-api/messaging/batch-outputs.md)
- [BatchResult, Fill, OrderUpdate](../../../06-api/grpc/) — `fob.matching.v1`

## Related Components

- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)
- [ledger](../../../05-components/ledger/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [observability-reporting](../../../05-components/observability-reporting/overview.md)

## Related Data

- [fills](../../../07-data/data-overview.md) (ClickHouse)
- [batch_results](../../../07-data/data-overview.md) (ClickHouse)
- [positions](../../../07-data/data-overview.md) (PostgreSQL)
