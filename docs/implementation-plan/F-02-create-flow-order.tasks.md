# Implementation Tasks: F-02 Create FlowOrder

## Source Artifacts

- Feature: [F-02 Create FlowOrder](../02-system/features/F-02-create-floworder/)
- Use Case: [UC-F02-01](../02-system/use-cases/UC-F02-01-create-flow-order/use-case.md)
- System Sequence: [SEQ-UC-F02-01-system](../02-system/use-cases/UC-F02-01-create-flow-order/sequences/SEQ-UC-F02-01-system.md)
- Service Sequence: [SEQ-F02-UC-F02-01-services](../05-components/sequences/SEQ-F02-UC-F02-01-services.md)
- Contracts: `fob.orders.v1.OrderFlowService`, `fob.risk.v1.RiskService.PreTradeCheck`, `fob.ledger.v1.LedgerService.Reserve`, [orders.normalized](../06-api/messaging/orders-normalized.md)
- Data: [`flow_orders`](../07-data/oltp-schema.md#таблица-flow_orders), [`accounts`](../07-data/oltp-schema.md#таблица-accounts), [`risk_limits`](../07-data/oltp-schema.md#таблица-risk_limits)
- IN-001 fragments: IN-001-FR-015, IN-001-FR-016 (FR-ORDER-001/002), IN-001-FR-028

## Preconditions

- [x] Feature exists
- [x] Use case exists
- [x] System-level sequence exists
- [x] Service-level sequence exists
- [x] Contracts exist (gRPC + Kafka)
- [x] Data objects exist (DDL в oltp-schema.md)
- [x] Acceptance criteria exist (см. README.md фичи, блок «Acceptance Criteria (IN-001)»)

## Tasks

### T-F02-001. PostgreSQL `flow_orders` migration

Target:

- Создать миграцию для таблицы `flow_orders` по DDL из [`docs/07-data/oltp-schema.md`](../07-data/oltp-schema.md#таблица-flow_orders).
- Индексы: `(user_id, status)`, `(symbol, status)`, partial `WHERE status IN ('active','partially_filled')`.

Acceptance:

- миграция выполняется на чистой БД;
- идентичная схема в dev/CI;
- rollback корректен.

### T-F02-002. Repository слой для FlowOrder

Target:

- `cpp/order_flow/src/infra/flow_orders_repository.{hpp,cpp}`.
- CRUD + idempotency по `client_order_id`.

Acceptance:

- unit tests против тестовой PG;
- идемпотентность подтверждена.

### T-F02-003. Pre-trade integration с Risk

Target:

- Гарантировать вызов `RiskService.PreTradeCheck` перед резервом в Ledger.
- Передавать решение пользователю (REJECT / THROTTLE / ACCEPT) с понятной причиной.

Acceptance:

- интеграционный тест Risk REJECT → клиент видит причину;
- THROTTLE возвращает скорректированные `q_rate`/`q_max`.

### T-F02-004. Ledger Reserve

Target:

- При ACCEPT: `LedgerService.Reserve(amount, order_id)` атомарно увеличивает `reserved_balance` и уменьшает `free_balance`.
- При cancel/expire: освобождение неисполненного остатка (см. F-03).

Acceptance:

- баланс никогда не отрицателен;
- идемпотентность по `order_id`.

### T-F02-005. Публикация `orders.normalized`

Target:

- Producer публикует событие в `orders.normalized` после успешного резерва.
- Partition key = `symbol`.

Acceptance:

- consumer Matching получает событие;
- replay даёт идентичный поток.

### T-F02-006. Preview VWAP/IS

Target:

- Endpoint preview (REST `POST /v1/flow-orders/preview`).
- Расчёт ожидаемого VWAP и IS (среднее, std) на основе текущих кривых и Market Data.

Acceptance:

- preview возвращается за p95 ≤ 200 ms;
- сходится с фактическим IS на backtest.

### T-F02-007. Тесты

Target:

- Unit: domain rules (валидация `p_low <= p_high`, `q_max > 0`).
- Integration: gRPC → Risk → Ledger → Kafka end-to-end на docker compose.
- E2E (`mvp_e2e`): создать FlowOrder, получить статус `active`, дождаться первого fill.

Acceptance:

- все три уровня зелёные в CI.

## Out of scope для этого таска

- Combo / portfolio orders (см. F-09).
- MM curve upsert (см. F-10).
- Amend / cancel (см. F-03).

## Next

После завершения T-F02-001..007 — обновить статус F-02 в [`coverage-matrix.md`](../traceability/coverage-matrix.md) до `complete`.
