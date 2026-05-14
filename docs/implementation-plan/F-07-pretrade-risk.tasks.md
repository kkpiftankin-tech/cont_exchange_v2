# Implementation Tasks: F-07 Pre-trade Risk Control

## Source Artifacts

- Feature: [F-07 Pre-trade Risk](../02-system/features/F-07-pretrade-risk/)
- Use Case: [UC-F07-01](../02-system/use-cases/UC-F07-01-pretrade-risk-check/use-case.md)
- System Sequence: [SEQ-UC-F07-01-system](../02-system/use-cases/UC-F07-01-pretrade-risk-check/sequences/SEQ-UC-F07-01-system.md)
- Service Sequence: [SEQ-F07-UC-F07-01-services](../05-components/sequences/SEQ-F07-UC-F07-01-services.md)
- Contracts: `fob.risk.v1.RiskService.PreTradeCheck`, [risk.alerts](../06-api/messaging/risk-alerts.md)
- Data: [`risk_limits`](../07-data/oltp-schema.md#таблица-risk_limits), [`accounts`](../07-data/oltp-schema.md#таблица-accounts), [`positions`](../07-data/oltp-schema.md#таблица-positions)
- IN-001 fragments: IN-001-FR-016 (FR-RISK-001), IN-001-FR-028

## Preconditions

- [x] Feature exists
- [x] Use case exists
- [x] System-level sequence exists
- [x] Service-level sequence exists
- [x] Contracts exist
- [x] Data objects exist
- [x] Acceptance criteria exist

## Tasks

### T-F07-001. PostgreSQL `risk_limits` migration

Target:

- Миграция по DDL из [`docs/07-data/oltp-schema.md`](../07-data/oltp-schema.md#таблица-risk_limits).
- Сидинг default лимитов на роль (`demo`, `client`).

Acceptance:

- миграция чистая;
- default limits применяются автоматически новым пользователям.

### T-F07-002. Логика проверки лимитов

Target:

- В `cpp/risk/src/app/risk_uc.cpp` реализовать:
  - `max_notional` — суммарный по всем активным заявкам пользователя;
  - `max_position` — по позиции в `symbol`;
  - `max_leverage` — на основе текущей маржи;
  - `max_order_rate` — окно в 60 сек;
  - `asset_whitelist` — `symbol IN ANY(whitelist)`;
  - `kill_switch` — флаг (global / by symbol).
- Возврат `RiskDecision`: ACCEPT / REJECT / RESIZE / HALT с причиной.

Acceptance:

- unit tests на каждое правило (false positive / false negative);
- p95 latency ≤ 50 ms на pre-trade check.

### T-F07-003. RESIZE / THROTTLE

Target:

- Поддержка решения RESIZE: вернуть скорректированные `q_rate` и `q_max`.
- API сервиса сохраняет совместимость с текущим клиентом.

Acceptance:

- если запрошенный `q_rate` превышает лимит — Risk возвращает скорректированное значение;
- клиент может принять корректировку.

### T-F07-004. Публикация `risk.alerts` для нестандартных решений

Target:

- Producer публикует событие в `risk.alerts` для REJECT / HALT / RESIZE с reason details.

Acceptance:

- alerts видны в Observability;
- audit trail полон.

### T-F07-005. Тесты

Target:

- Unit: каждое правило отдельно.
- Integration: gRPC PreTradeCheck с реальной PG.
- Property-based: monotonicity (большее `notional` не должно становится более разрешённым).

Acceptance:

- все три уровня зелёные.

## Out of scope

- Post-trade монитор и ликвидации (F-08).
- Kill-switch UI (F-16) — реализуется как Operator API; здесь только consumer флага.

## Next

После завершения — обновить статус F-07 в [`coverage-matrix.md`](../traceability/coverage-matrix.md) до `complete`.
