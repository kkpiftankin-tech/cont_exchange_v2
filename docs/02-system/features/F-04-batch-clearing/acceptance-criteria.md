# Acceptance Criteria — F-04

## Функциональные

| #    | Критерий                                                         | Статус                                                                                  |
| ---- | ---------------------------------------------------------------- | --------------------------------------------------------------------------------------- |
| AC-1 | Solver запускается каждые `batchIntervalMs` (из `solverconfig`)  | ✅ `PostgresSolverConfigRepository` читает из БД; fallback на env                       |
| AC-2 | На каждый батч собран список активных FlowOrder                  | ⚠️ Код готов (`PostgresFlowOrderRepository`); ждёт DDL `flow_orders` в шаге 3            |
| AC-3 | Запрошены reference prices у Market Data Service                 | ✅ `MarketDataClient` gRPC                                                              |
| AC-4 | Вычислены clearPrices и executedRates для всех инструментов      | ✅ `ContinuousClearingSolver` (Eigen3 Sparse Cholesky) в [solver_impl.cpp](../../../../cpp/matching/src/domain/solver_impl.cpp) |
| AC-5 | Для каждого FlowOrder сформирован FillEvent с execQty, execPrice | ✅ `batch_result_to_fill_events.cpp` заполняет execQty, execPrice, liquidity_source, fees, fill_id |
| AC-6 | BatchResult публикуется в Kafka `batch.outputs`                  | ✅ `BatchOutputsProducer`                                                               |
| AC-7 | FillEvent публикуются в Kafka `fills` (отдельный топик)          | ✅ `batch_outputs_producer.cpp:42` produce("fills", ...)                                 |
| AC-8 | BatchResult и FillEvent сохраняются в ClickHouse                 | ❌ Планируется (PR feat/f04-clickhouse, шаг 4)                                          |
| AC-9 | Web UI получает обновления по WebSocket                          | ❌ Frontend scope, отдельный трек                                                       |

## Нефункциональные

| #     | Критерий                                                       | Статус                                                                                                          |
| ----- | -------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| AC-10 | Median `solveTimeMs` ≤ 500 ms                                  | ✅ `SolverMetrics::ObserveBatch` измеряет; Prometheus `matching_solver_solve_time_ms`                          |
| AC-11 | p95 `solveTimeMs` ≤ 1000 ms                                    | ✅ Те же метрики; SLA тест в `f04_sla_test.cpp`                                                                |
| AC-12 | `residualNorm` ≤ `tolerance`                                   | ✅ `solver_impl.cpp:525` вычисляется; Prometheus `matching_solver_residual_norm`                               |
| AC-13 | Детерминированность: одинаковые входы → одинаковый BatchResult | ✅ Eigen Cholesky + детерминистский порядок входа; покрытие в `f04_full_cycle_test.cpp`                        |
| AC-14 | При residualNorm > tolerance → BatchResult с error flag + alert | ⚠️ Метрика выставляется, alert через external Prometheus rule                                                  |
| AC-15 | При solveTimeMs > SLA → alert (опц. kill-switch)               | ⚠️ Метрика выставляется, alert через external Prometheus rule; kill-switch — F-16 scope                        |

## Unit-тесты (из спецификации F-04 §5.1)

Все U1-U10 покрыты в [f04_solver_u1_u10_test.cpp](../../../../cpp/matching/tests/domain/f04_solver_u1_u10_test.cpp):

| #   | Тест                                | Описание                                                          | Статус |
| --- | ----------------------------------- | ----------------------------------------------------------------- | ------ |
| U1  | Симметричные buy/sell, один актив   | clearPrice ≈ mid, executedRates симметричны                       | ✅     |
| U2  | Разные qRate                        | clearPrice ≈ mid, лимитировано меньшим qRate                      | ✅     |
| U3  | Q_max ограничивает                  | один ордер уходит в FILLED, другой остаётся ACTIVE                | ✅     |
| U4  | Несовпадающие диапазоны (граница)   | clearPrice на границе пересечения                                 | ✅     |
| U5  | Непересекающиеся диапазоны          | executedRates ≈ 0                                                 | ✅     |
| U6  | Portfolio orders (BTC+ETH)          | зеркальные assetLegs                                              | ✅     |
| U7  | Многоногие vs одна нога             | эквивалентность результата                                        | ✅     |
| U8  | Сходимость                          | residualNorm ≤ tolerance, iterations < max                        | ✅     |
| U9  | Несходимость                        | stoppedByMaxIterations, residualNorm > tolerance                  | ✅     |
| U10 | Инвариант диапазона                 | min(pL) ≤ clearPrice ≤ max(pH)                                    | ✅     |

## Integration

| #  | Тест                                                          | Файл                                                                                          | Статус |
| -- | ------------------------------------------------------------- | --------------------------------------------------------------------------------------------- | ------ |
| I1 | Полный цикл: FlowOrder → batch.outputs → fills → ClickHouse   | [f04_full_cycle_test.cpp](../../../../cpp/matching/tests/domain/f04_full_cycle_test.cpp)      | ⚠️ Mocked ClickHouse в тесте; реальная ingestion в шаге 4 |
| I2 | SLA нагрузка: 1000 FlowOrder, p95 solveTimeMs ≤ 50 ms         | [f04_sla_test.cpp](../../../../cpp/matching/tests/domain/f04_sla_test.cpp)                    | ✅     |

> Легенда: ✅ выполнено, ⚠️ частично, ❌ не выполнено.
