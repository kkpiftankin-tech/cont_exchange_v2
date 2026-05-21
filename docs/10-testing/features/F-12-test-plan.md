---
id: DOC-TEST-F-12
phase: 10-testing
status: draft
owner: core-team
source:
  - IN-005 §10 «Тестовые кейсы»
related:
  - docs/02-system/features/F-12-execution-hedge/
  - docs/implementation-plan/F-12-execution-hedge.tasks.md
  - docs/02-system/non-functional-requirements.md
---

# F-12 Execution Hedge — план тестирования

Полный спецификационный источник: [`incoming-docs/2026-05-20-F-12-execution-hedge-v1.md`](../../../incoming-docs/2026-05-20-F-12-execution-hedge-v1.md) §10.

## 1. Юнит-тесты (U1-U10)

Тестируется бизнес-логика без Kafka / PostgreSQL / venue (через mocks / `VenueSimAdapter`).

### U1. Базовый happy path (FILLED)

| Аспект | Описание |
| --- | --- |
| Сценарий | Один ExecutionIntent с targetQty=1.0 BTC, urgency=MEDIUM, allowed_venues=[venue_sim]. VenueSim возвращает FILLED. |
| Входы | ExecutionIntent{target_qty=1.0, urgency=MEDIUM, reference_mid=50_000} |
| Ожидание | HedgeFlow=COMPLETED, filled_qty=1.0, avg_fill_price≈50_000, hedge_pnl=0 (zero slippage simulator) |
| Файл (existing) | [`cpp/venues/tests/venue_sim_adapter_test.cpp`](../../../cpp/venues/tests/venue_sim_adapter_test.cpp) — частично; полный U1 — TODO `cpp/venues/tests/it_f12_u1_test.cpp` |
| Status | ⚠️ partial coverage |

### U2. Partial fill + retry

| Аспект | Описание |
| --- | --- |
| Сценарий | Первый child_order на venue A заполнен на 60% (POST_ONLY); retry с urgency=HIGH на venue B заполняет остаток. |
| Ожидание | Два child_orders: A status=PARTIALLY_FILLED filled_qty=0.6; B status=FILLED filled_qty=0.4. HedgeFlow=COMPLETED. |
| Файл | TODO `cpp/venues/tests/u_f12_u2_partial_retry_test.cpp` |
| Status | ❌ |

### U3. Overfill guard

| Аспект | Описание |
| --- | --- |
| Сценарий | Late fill on retry заходит после того, как Adapter уже посчитал HedgeFlow=COMPLETED. `accumulatedFilledQty > targetQty + overfillThreshold`. |
| Ожидание | ExecutionReport(status=OVERFILL_GUARD), все open child_orders cancelled. |
| Файл | TODO `cpp/venues/tests/u_f12_u3_overfill_test.cpp` |
| Status | ❌ |

### U4. Rejection + fallback

| Аспект | Описание |
| --- | --- |
| Сценарий | Venue A REJECTED (insufficient_liquidity), Execution Planning re-routes на venue B; B FILLED. |
| Ожидание | child_orders: A=REJECTED, B=FILLED. HedgeFlow=COMPLETED. |
| Файл | TODO `cpp/venues/tests/u_f12_u4_rejection_fallback_test.cpp` (требует execution-planning impl) |
| Status | ❌ |

### U5. Timeout → UNDERFILLED

| Аспект | Описание |
| --- | --- |
| Сценарий | venue B PARTIALLY_FILLED после `timeout_ms`. `gap > reconciliationGapThreshold` и нет healthy fallback. |
| Ожидание | child_order=PARTIALLY_FILLED + CANCELLED open остаток; HedgeFlow=UNDERFILLED + risk.alert(HEDGE_UNDERFILL). |
| Файл | TODO `cpp/venues/tests/u_f12_u5_timeout_test.cpp` |
| Status | ❌ |

### U6. Urgency → orderType маппинг

| Аспект | Описание |
| --- | --- |
| Сценарий | Test matrix: для каждой urgency проверить отображение в `ExecutionStrategy` и `TimeInForce`. |
| Ожидание | LOW → POST_ONLY/LIMIT, GTC; MEDIUM → LIMIT, GTC/IOC; HIGH → MARKET/LIMIT, IOC. |
| Файл (existing) | [`cpp/matching/tests/app/execution_intent_builder_test.cpp`](../../../cpp/matching/tests/app/execution_intent_builder_test.cpp) — частично |
| Status | ⚠️ partial |

### U7. Reconciliation: filledQty vs targetQty

| Аспект | Описание |
| --- | --- |
| Сценарий | Test matrix: gap ∈ {0, threshold-eps, threshold+eps, large} → expected status. |
| Ожидание | gap ≤ threshold → COMPLETED; gap > threshold + есть fallback → retry; иначе UNDERFILLED. |
| Файл | TODO `cpp/venues/tests/u_f12_u7_reconciliation_test.cpp` |
| Status | ❌ |

### U8. clientOrderId idempotency

| Аспект | Описание |
| --- | --- |
| Сценарий | Двойная обработка одного ExecutionIntent. Проверка: один HedgeFlow + один набор child_orders. |
| Ожидание | Второй вызов — no-op (нет duplicate rows в hedgeflows). |
| Файл | TODO `cpp/venues/tests/u_f12_u8_idempotency_test.cpp` (требует postgres hedgeflows) |
| Status | ❌ |

### U9. Multi-venue split

| Аспект | Описание |
| --- | --- |
| Сценарий | targetQty=10 BTC; venues=[A,B,C] с L(A)=50, L(B)=30, L(C)=20 → qty=[5, 3, 2]. |
| Ожидание | Routing plan совпадает с формулой; учитывает lot rounding. |
| Файл (existing) | [`cpp/matching/tests/app/external_venue_filter_test.cpp`](../../../cpp/matching/tests/app/external_venue_filter_test.cpp) — фильтрация; формула split — TODO `cpp/matching/tests/app/routing_plan_test.cpp` |
| Status | ⚠️ partial |

### U10. Pre-hedge risk check rejection

| Аспект | Описание |
| --- | --- |
| Сценарий | RiskService.PreHedgeCheck возвращает REJECT (NOTIONAL_LIMIT). |
| Ожидание | HedgeFlow=RISK_REJECTED, никаких child_orders, risk.alert(HEDGE_REJECTED) published. |
| Файл | TODO `cpp/matching/tests/app/pre_hedge_check_test.cpp` (требует PreHedgeCheck RPC) |
| Status | ❌ |

## 2. Integration-тесты (IT-1..IT-7)

Тестируется связка `venue-execution-adapter` + `execution-planning` + `risk-manager` + `ledger` + Kafka + PostgreSQL + ClickHouse.

### IT-1. Полный цикл E2E COMPLETED → Ledger

1. docker-compose up.
2. Опубликовать ExecutionIntent в Kafka `execution.intents`.
3. Через `timeout_ms` проверить:
   - Kafka `execution.venue` содержит ExecutionReport(FILLED).
   - PostgreSQL `hedgeflows` row status=COMPLETED, корректные агрегаты.
   - PostgreSQL `child_orders` rows status=FILLED.
   - ClickHouse `execution_reports` rows = N (по числу child_orders + дополнительных reports).
   - Ledger positions provider'а обновлены, hedge_pnl рассчитан.

Файл TODO: `cpp/venues/tests/it_f12_full_cycle_test.cpp`.

Status: ❌.

### IT-2. Partial fill + retry полный цикл

Сценарий: venue A POST_ONLY с partial fill → reconciliation создаёт retry с urgency=HIGH → venue B FILLED.

Файл TODO: `cpp/venues/tests/it_f12_partial_retry_test.cpp`. Status: ❌.

### IT-3. Rejection + fallback E2E

Файл TODO: `cpp/venues/tests/it_f12_rejection_fallback_test.cpp`. Status: ❌.

### IT-4. Timeout + UNDERFILLED + risk alert

Файл TODO: `cpp/venues/tests/it_f12_timeout_test.cpp`. Status: ❌.

### IT-5. SLA latency ≤ 100 ms CEX

1. CEX adapter с realistic-ish backoff (skip `network_delay_ms=20-50`).
2. Прогнать 1000 ExecutionIntent последовательно.
3. Замерить p50 / p95 / p99 latency от publish ExecutionIntent до first child order ack.

Критерии: p95 ≤ 100 ms (per F12-10).

Файл TODO: `cpp/venues/tests/it_f12_sla_test.cpp`. Status: ❌.

### IT-6. Нагрузочный тест throughput

Целевая нагрузка (IN-005 §8):
- Sustained: 50 HedgeFlow/s.
- Peak: 100 HedgeFlow/s.

Критерии: 0 dropped events, consumer lag ≤ 5 s, no OOM.

Файл TODO: `tests/performance/f12_load_test.py`. Status: ❌.

### IT-7. Backtest parity VenueSim vs production

1. Прогнать заранее заготовленный сценарий через `VenueSimAdapter`.
2. Прогнать тот же сценарий через "production-like" adapter (например, `simulated_venue_adapter` с deterministic seed).
3. Сравнить ExecutionReport побайтово.

Файлы (existing):
- [`cpp/venues/tests/backtest_parity_check_test.cpp`](../../../cpp/venues/tests/backtest_parity_check_test.cpp)
- [`cpp/venues/tests/backtest_synthetic_scenarios_test.cpp`](../../../cpp/venues/tests/backtest_synthetic_scenarios_test.cpp)

Status: ✅.

## 3. Ручные тесты

### 3.1. Operator manual hedge

1. Войти в Admin UI как `operator`.
2. POST `/api/v1/hedge/intents/manual` с `provider_id=demo`, `symbol=BTC/USDT`, `side=SELL`, `target_qty=0.5`, `urgency=MEDIUM`.
3. Проверить:
   - HedgeFlow Monitor показывает новую сессию (status=OPEN → COMPLETED).
   - Execution Timeline содержит ExecutionReport.
   - Hedge PnL Dashboard обновлён.

### 3.2. Manual override при отказе auto-hedge

1. Установить риск-полу-блокер (kill-switch для AUTO_BATCH).
2. Verify, что auto-hedge генерирует risk.alert(HEDGE_REJECTED).
3. Оператор выполняет manual override.

### 3.3. Reconciliation Alerts

1. Эмулировать timeout (VenueSim с `force_partial_fill=0.5`, `force_timeout=true`).
2. Проверить:
   - HedgeFlow=UNDERFILLED.
   - risk.alert опубликован.
   - Reconciliation Alerts экран показывает запись.

## 4. Coverage matrix

| AC | Unit tests | Integration tests | Status |
| --- | --- | --- | --- |
| F12-1 | (нет dedicated test policy wiring) | IT-1 | ⚠️ |
| F12-2 | U6 (partial) | — | ⚠️ |
| F12-3 | U9 (partial) | IT-1 | ❌ |
| F12-4 | U3 | — | ❌ |
| F12-5 | U5, U7 | IT-4 | ❌ |
| F12-6 | execution_report_producer_test | IT-1 | ⚠️ |
| F12-7 | U8 | IT-1 | ❌ |
| F12-8 | U10 | — | ❌ |
| F12-9 | — | IT-7 | ✅ |
| F12-10 | — | IT-5 | ❌ |
| F12-11 | — | IT-1 (HedgePnL расчёт) | ❌ |
| F12-12 | U4 | IT-3 | ❌ |

## 5. Подключение к DoD

Чек-лист DoD F-12 ссылается на тесты из этого плана. См. [implementation-plan/F-12-execution-hedge.tasks.md → DoD](../../implementation-plan/F-12-execution-hedge.tasks.md#definition-of-done).

## Source Fragments

- IN-005 §10.1 «Unit-тесты U1-U10»
- IN-005 §10.2 «Integration-тесты IT-1..IT-7»
- IN-005 §8 «Non-functional» (SLA bounds для IT-5, IT-6)
