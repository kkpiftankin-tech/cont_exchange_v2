# Acceptance Criteria — F-12 Execution Hedge

Источник: IN-005 §7 «Acceptance Criteria» + §8 «Non-functional» + §10 «Тестовые кейсы».

## Функциональные

| #     | Критерий                                                                                          | Статус |
| ----- | ------------------------------------------------------------------------------------------------- | ------ |
| F12-1 | ExecutionIntent генерируется автоматически при `|netQty| >= hedgeTriggerThreshold[symbol]` или `triggerNotional >= thresholdNotional` после batch clearing | ⚠️ matching содержит [`HedgeTriggerPolicy`](../../../../cpp/matching/src/app/hedge_trigger_policy.cpp), [`PositionSnapshotCalculator`](../../../../cpp/matching/src/app/position_snapshot_calculator.cpp), [`ExecutionIntentBuilder`](../../../../cpp/matching/src/app/execution_intent_builder.cpp), [`HedgeExecutionIntentsPublisher`](../../../../cpp/matching/src/app/hedge_execution_intents_publisher.cpp); требуется wiring в `RunBatchUseCase` и продовая конфигурация порогов |
| F12-2 | urgency LOW/MEDIUM/HIGH маппится на ExecutionStrategy и TimeInForce: LOW->POST_ONLY/LIMIT GTC; MEDIUM->LIMIT GTC|IOC; HIGH->MARKET|LIMIT IOC | ⚠️ enum-ы определены в [`execution.proto`](../../../../contracts/proto/fob/execution/v1/execution.proto); маппинг политики в `ExecutionIntentBuilder` присутствует, нужна полная таблица в `policy_config` |
| F12-3 | Multi-venue routing: `qty[v] = L(v) / SumLv' L(v')` * targetQty, исключение venue со статусом `!= CONNECTED`, fallback на оставшиеся | ❌ `IExecutionPlanningUseCases` определён как абстрактный интерфейс, конкретной реализации нет. F-11 публикует `venue.liquidity.fob` и `venue.health`, но Planning не подписан на эти топики |
| F12-4 | Overfill guard: при `accumulatedFilledQty - targetQty > overfillThreshold` -> cancel остальных child orders + publish ExecutionReport(OVERFILL_GUARD) | ❌ enum [`EXECUTION_REPORT_STATUS_OVERFILL_GUARD`](../../../../contracts/proto/fob/execution/v1/execution.proto) определён в proto; логика обнаружения и cancel не реализована в `ExecuteOnVenue` |
| F12-5 | Reconciliation после `hedgeTimeoutMs`: при `gap > reconciliationGapThreshold` и наличии fallback venue — retry с urgency=HIGH; иначе risk.alert(HEDGE_UNDERFILL) + status=UNDERFILLED | ❌ нет reconciliation loop в venues; нет таймера timeoutMs |
| F12-6 | Все ExecutionReport публикуются в Kafka `execution.venue` (partitionKey=hedge_flow_id) | ⚠️ producer существует в [`execution_report_producer.cpp`](../../../../cpp/venues/src/infra/execution_report_producer.cpp); используется legacy-имя `execution.reports` (Conflict Note); требуется switch на `execution.venue` |
| F12-7 | HedgeFlow + child_orders сохраняются в PostgreSQL; execution_reports — в ClickHouse | ❌ DDL отсутствует: ни в [`infra/postgres/init.sql`](../../../../infra/postgres/init.sql), ни в [`infra/clickhouse/init.sql`](../../../../infra/clickhouse/init.sql). Требуется T-F12-201, T-F12-202, T-F12-203 |
| F12-8 | Pre-hedge risk check через `RiskService.PreHedgeCheck`: 3 проверки (`targetNotional <= maxNotionalPerHedge`, `currentHedgeExposure + targetQty <= hedgeExposureLimit`, `expectedSlippage <= maxSlippage[urgency]`) | ❌ метод `PreHedgeCheck` отсутствует в [`risk.proto`](../../../../contracts/proto/fob/risk/v1/risk.proto); требуется extension |
| F12-9 | Backtest-режим: при подмене EVC на `VenueSimAdapter` execution logic идентична production (parity tests) | ✅ [`venue_sim_adapter.cpp`](../../../../cpp/venues/src/infra/venue_sim_adapter.cpp) + [`backtest_parity_check_test.cpp`](../../../../cpp/venues/tests/backtest_parity_check_test.cpp); существуют [`backtest_synthetic_scenarios_test.cpp`](../../../../cpp/venues/tests/backtest_synthetic_scenarios_test.cpp) |
| F12-10 | p95 latency от ExecutionIntent до first child order <= 100 ms (CEX); <= 1000 ms (DEX/AMM) | ❌ метрики не настроены; нет integration-теста IT-5 |
| F12-11 | HedgePnL рассчитывается Settlement Ledger и доступен в UI через REST `GET /api/v1/hedge/flows/{id}` | ❌ [`ledger_uc.cpp`](../../../../cpp/ledger/src/app/ledger_uc.cpp) логирует ExecutionReport, не считает HedgePnL; REST API определён только в OpenAPI ([`contracts/openapi/fob/hedge/v1/api/hedge.yaml`](../../../../contracts/openapi/fob/hedge/v1/api/hedge.yaml)), нет реализации в gateway |
| F12-12 | Rejection fallback: при ExecutionReport(REJECTED) Execution Planning исключает venue и формирует новый routing plan на оставшиеся | ❌ требует Execution Planning (F12-3 + reconciliation hook); сейчас при reject venues просто публикует ExecutionReport без retry |

## Нефункциональные

| #      | Критерий                                                                                              | Статус |
| ------ | ----------------------------------------------------------------------------------------------------- | ------ |
| F12-N1 | Fill rate для urgency HIGH: >= 95% от targetQty за hedgeTimeoutMs                                     | ❌ нет нагрузочного теста IT-6 |
| F12-N2 | Slippage: LOW <= 5 bps, MEDIUM <= 15 bps, HIGH <= 30 bps                                              | ❌ slippageBps вычисляется в ExecutionReport (поле есть в proto), но политика maxSlippage[urgency] не сконфигурирована |
| F12-N3 | Reconciliation gap: <= 0.01% targetQty                                                                | ❌ нет reconciliation loop |
| F12-N4 | Latency Intent -> first child order: CEX p95 <= 100 ms, DEX/AMM p95 <= 1000 ms                        | ❌ нет метрик |
| F12-N5 | HedgeFlow audit trail: 100% HedgeFlow имеют полный trail в PostgreSQL + ClickHouse                    | ❌ DDL отсутствует |
| F12-N6 | Overfill guard: 0 случаев overfill > 1% targetQty в production                                        | ❌ overfill detection не реализован |
| F12-N7 | Backtest parity: execution logic идентична production при замене EVC на VenueSim                     | ✅ [`backtest_parity_check_test.cpp`](../../../../cpp/venues/tests/backtest_parity_check_test.cpp) |
| F12-N8 | Risk pre-check: 0 случаев исполнения ExecutionIntent без проверки Risk Manager                        | ❌ PreHedgeCheck RPC не реализован |

## Unit-тесты (IN-005 §10.1, U1-U10)

| #   | Тест                                | Описание                                                          | Существующий код-файл                                                                                       | Статус |
| --- | ----------------------------------- | ----------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- | ------ |
| U1  | Базовый happy path (FILLED)         | ExecutionIntent -> child order -> FILLED -> HedgeFlow=COMPLETED   | [`venue_sim_adapter_test.cpp`](../../../../cpp/venues/tests/venue_sim_adapter_test.cpp)                     | ⚠️ покрытие только на уровне VenueSim |
| U2  | Partial fill + retry                | child order partial -> remainingQty > 0 -> retry intent           | (нет файла)                                                                                                 | ❌     |
| U3  | Overfill guard                      | accumulated > target -> cancel all + ExecutionReport(OVERFILL_GUARD) | (нет файла)                                                                                              | ❌     |
| U4  | Rejection + fallback                | venue A rejects -> Planning excludes A -> route to venue B        | (нет файла, требует Execution Planning)                                                                     | ❌     |
| U5  | Timeout -> UNDERFILLED              | hedgeTimeoutMs истёк -> CancelOrder + ExecutionReport(EXPIRED) -> UNDERFILLED | (нет файла)                                                                                       | ❌     |
| U6  | Urgency -> orderType маппинг        | LOW->POST_ONLY/LIMIT, MEDIUM->LIMIT|IOC, HIGH->MARKET|IOC         | [`execution_intent_builder_test.cpp`](../../../../cpp/matching/tests/app/execution_intent_builder_test.cpp) | ⚠️ покрытие частичное |
| U7  | Reconciliation: filledQty vs target | gap > threshold -> retry или alert; иначе COMPLETED               | (нет файла)                                                                                                 | ❌     |
| U8  | clientOrderId idempotency           | повторная отправка того же intent не создаёт дубль HedgeFlow      | (нет файла; зависит от PostgreSQL hedgeflows DDL)                                                           | ❌     |
| U9  | Multi-venue split                   | routing plan `qty[v] = L(v)/SumL * targetQty`                     | [`external_venue_filter_test.cpp`](../../../../cpp/matching/tests/app/external_venue_filter_test.cpp)       | ⚠️ покрывает фильтрацию; не покрывает формулу split |
| U10 | Pre-hedge risk check rejection      | PreHedgeCheck возвращает REJECT -> HedgeFlow=RISK_REJECTED        | (нет файла; зависит от PreHedgeCheck RPC)                                                                   | ❌     |

## Integration-тесты (IN-005 §10.2, IT-1..IT-7)

| #    | Тест                                                          | Файл / TODO                                                                                                                | Статус |
| ---- | ------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- | ------ |
| IT-1 | Полный цикл E2E: Intent -> child orders -> Reports -> Ledger | TODO `cpp/venues/tests/it_f12_full_cycle_test.cpp`                                                                         | ❌     |
| IT-2 | Partial fill + retry полный цикл                              | TODO `cpp/venues/tests/it_f12_partial_retry_test.cpp`                                                                      | ❌     |
| IT-3 | Rejection + fallback E2E                                      | TODO `cpp/venues/tests/it_f12_rejection_fallback_test.cpp`                                                                 | ❌     |
| IT-4 | Timeout + UNDERFILLED + risk alert                            | TODO `cpp/venues/tests/it_f12_timeout_test.cpp`                                                                            | ❌     |
| IT-5 | SLA latency <= 100 ms CEX                                     | TODO `cpp/venues/tests/it_f12_sla_test.cpp`                                                                                | ❌     |
| IT-6 | Нагрузочный тест throughput: 50 HedgeFlow/s sustained, 100/s peak | TODO `tests/performance/f12_load_test.py`                                                                              | ❌     |
| IT-7 | Backtest parity VenueSim vs production                        | [`backtest_parity_check_test.cpp`](../../../../cpp/venues/tests/backtest_parity_check_test.cpp), [`backtest_synthetic_scenarios_test.cpp`](../../../../cpp/venues/tests/backtest_synthetic_scenarios_test.cpp) | ✅     |

## Definition of Done (IN-005 §11)

См. [implementation-plan/F-12-execution-hedge.tasks.md](../../../implementation-plan/F-12-execution-hedge.tasks.md#definition-of-done). Ключевые пункты (18 items):

1. Публикация ExecutionIntent с полным контрактом — частично (matching code, без routing/idempotency wiring).
2. Execution Planning с routing plan — не реализовано.
3. PreHedgeCheck (3 проверки) — не реализовано.
4. Venue Execution Adapter + HedgeFlow + child_orders state — частично (есть `ExecuteOnVenue`, нет persistence).
5. Публикация ExecutionReport в `execution.venue` — частично (топик `execution.reports`).
6. Ledger потребляет execution.venue, обновляет позиции + HedgePnL — не реализовано (только логирование).
7. ClickHouse `execution_reports` (retention 90+ дней) — DDL отсутствует.
8. Unit U1-U10 + Integration IT-1..IT-7 — 1/10 unit, 1/7 integration.
9. p95 latency CEX <= 100 ms — метрики не настроены.
10. Backtest parity confirmed — ✅
11. UI: HedgeFlow Monitor, Execution Live Feed, Hedge PnL Dashboard — REST API в OpenAPI, без backend.
12. Метрики и алерты в Observability — не настроены.
13. Operator runbook для инцидентов — не написан.
14. Архитектурная документация (этот PR) — ✅ docs-complete.
15. Postgres DDL hedgeflows + child_orders — ❌.
16. ClickHouse DDL execution_reports — ❌.
17. Kafka topics execution.intents + execution.venue созданы в `infra/kafka/create_topics.sh` — ✅.
18. Conflict Notes по существующим расхождениям зафиксированы — ✅ см. [feature.yaml](feature.yaml#knownIssues).

> Легенда: ✅ выполнено, ⚠️ частично, ❌ не выполнено.
