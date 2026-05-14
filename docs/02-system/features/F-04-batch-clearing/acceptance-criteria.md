# Acceptance Criteria — F-04

## Функциональные

| #    | Критерий                                                         | Статус                                   |
| ---- | ---------------------------------------------------------------- | ---------------------------------------- |
| AC-1 | Solver запускается каждые `batchIntervalMs` (из `solverconfig`)  | ⚠️ Только из env, не из БД               |
| AC-2 | На каждый батч собран список активных FlowOrder                  | ✅ Из Kafka (не из PostgreSQL)            |
| AC-3 | Запрошены reference prices у Market Data Service                 | ❌ Не реализовано                         |
| AC-4 | Вычислены clearPrices и executedRates для всех инструментов      | ⚠️ per-order midpoint, не единая цена    |
| AC-5 | Для каждого FlowOrder сформирован FillEvent с execQty, execPrice | ⚠️ Без liquidity_source / fees / fill_id |
| AC-6 | BatchResult публикуется в Kafka `batch.outputs`                  | ✅                                        |
| AC-7 | FillEvent публикуются в Kafka `fills` (отдельный топик)          | ❌ Используется один топик                |
| AC-8 | BatchResult и FillEvent сохраняются в ClickHouse                 | ❌ Не реализовано                         |
| AC-9 | Web UI получает обновления по WebSocket                          | ❌ Не реализовано (vне cpp/)              |

## Нефункциональные

| # | Критерий | Статус |
|---|---|---|
| AC-10 | Median `solveTimeMs` ≤ 500 ms | ❌ Не измеряется (захардкожен 1) |
| AC-11 | p95 `solveTimeMs` ≤ 1000 ms | ❌ Не измеряется |
| AC-12 | `residualNorm` ≤ `tolerance` | ❌ Не вычисляется (захардкожен 0) |
| AC-13 | Детерминированность: одинаковые входы → одинаковый BatchResult | ⚠️ Не гарантировано (unordered_map iteration) |
| AC-14 | При residualNorm > tolerance → BatchResult с error flag + alert | ❌ Не реализовано |
| AC-15 | При solveTimeMs > SLA → alert (опц. kill-switch) | ❌ Не реализовано |

## Unit-тесты (из спецификации)

| # | Тест | Описание | Статус |
|---|---|---|---|
| U1 | Симметричные buy/sell, один актив | clearPrice ≈ mid, executedRates симметричны | ❌ |
| U2 | Разные qRate | clearPrice ≈ mid, лимитировано меньшим qRate | ❌ |
| U3 | Q_max ограничивает | один ордер уходит в FILLED, другой остаётся ACTIVE | ❌ |
| U4 | Несовпадающие диапазоны (граница) | clearPrice на границе пересечения | ❌ |
| U5 | Непересекающиеся диапазоны | executedRates ≈ 0 | ❌ |
| U6 | Portfolio orders (BTC+ETH) | зеркальные assetLegs | ❌ |
| U7 | Многоногие vs одна нога | эквивалентность результата | ❌ |
| U8 | Сходимость | residualNorm ≤ tolerance, iterations < max | ❌ |
| U9 | Несходимость | stoppedByMaxIterations, residualNorm > tolerance | ❌ |
| U10 | Инвариант диапазона | min(pL) ≤ clearPrice ≤ max(pH) | ❌ |

## Integration

| # | Тест | Статус |
|---|---|---|
| I1 | Полный цикл: FlowOrder → batch.outputs → fills → ClickHouse | ❌ |
| I2 | SLA нагрузка: 1000 FlowOrder, p95 solveTimeMs ≤ 50 ms | ❌ |

> Легенда: ✅ выполнено, ⚠️ частично, ❌ не выполнено.
