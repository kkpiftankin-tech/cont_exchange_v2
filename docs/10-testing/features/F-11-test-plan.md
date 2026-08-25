---
id: DOC-TEST-F-11
phase: 10-testing
status: draft
owner: core-team
source:
  - IN-004 §«Тестовые кейсы»
related:
  - docs/02-system/features/F-11-external-venues-lob-to-fob/
  - docs/implementation-plan/F-11-external-venues.tasks.md
---

# F-11 External Venues / LOB → FOB — план тестирования

Полный спецификационный источник: [`incoming-docs/2026-05-20-F-11-external-venues-v1.md`](../../../incoming-docs/2026-05-20-F-11-external-venues-v1.md) §«Тестовые кейсы».

Все тесты живут под `cpp/venues/tests/`, `cpp/venue_health/tests/`, и `Testing/f11_*.{sh,py}`.

## 1. Юнит-тесты (IN-004 §«Unit-тесты»)

| #   | Тест                                                                  | Файл                                                                                                                  | Статус |
| --- | --------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- | ------ |
| U1  | Нормализация простого стакана CEX (5 bid + 5 ask)                     | [cpp/venues/tests/normalize_snapshot_test.cpp](../../../cpp/venues/tests/normalize_snapshot_test.cpp)                  | ✅     |
| U2  | Нормализация пустого стакана → `status=empty`                          | [cpp/venues/tests/snapshot_status_test.cpp](../../../cpp/venues/tests/snapshot_status_test.cpp)                        | ✅     |
| U3  | Нормализация DEX state (`sqrtPriceX96`, `tick`, `liquidity` для Uniswap v3) | [cpp/venues/tests/amm_pool_extractor_test.cpp](../../../cpp/venues/tests/amm_pool_extractor_test.cpp)                  | ✅     |
| U4  | LOB-FOB Level 1 для симметричного стакана                              | [cpp/venues/tests/depth_curve_builder_test.cpp](../../../cpp/venues/tests/depth_curve_builder_test.cpp)                | ✅     |
| U5  | LOB-FOB с учётом комиссий                                              | [cpp/venues/tests/depth_curve_builder_test.cpp](../../../cpp/venues/tests/depth_curve_builder_test.cpp)                | ⚠️ Проверить покрытие fee-сценариев в фикстурах |
| U6  | LOB-FOB с минимальным объёмом (фильтрация пылевых уровней)             | [cpp/venues/tests/depth_canonicalizer_test.cpp](../../../cpp/venues/tests/depth_canonicalizer_test.cpp)                | ✅     |
| U7  | Regularized L2 (выпуклость, монотонность, `epsilon2=0`)                | [cpp/venues/tests/depth_curve_regularization_test.cpp](../../../cpp/venues/tests/depth_curve_regularization_test.cpp)  | ✅     |
| U8  | Calibrated L3 (учёт execution reports, обновление `epsilon3`)          | —                                                                                                                     | ❌ Unit-теста нет; покрыт в integration `Testing/f11_test4_quality.sh` |
| U9  | DEX/AMM virtual LOB                                                    | [cpp/venues/tests/amm_virtual_lob_test.cpp](../../../cpp/venues/tests/amm_virtual_lob_test.cpp)                        | ✅     |
| U10 | Circuit breaker: `CLOSED → OPEN`                                       | [cpp/venue_health/tests/domain/circuit_breaker_test.cpp](../../../cpp/venue_health/tests/domain/circuit_breaker_test.cpp) | ✅     |
| U11 | Circuit breaker: `OPEN → HALF_OPEN → CLOSED`                           | [cpp/venue_health/tests/domain/circuit_breaker_test.cpp](../../../cpp/venue_health/tests/domain/circuit_breaker_test.cpp) | ✅     |
| U12 | Stale detection (пересечение порога свежести)                          | [cpp/venues/tests/snapshot_status_test.cpp](../../../cpp/venues/tests/snapshot_status_test.cpp)                        | ✅     |

Дополнительные unit-тесты, не в IN-004, но в репо:

- [depth_curve_dual_test.cpp](../../../cpp/venues/tests/depth_curve_dual_test.cpp) — проверка дуального слоя $S^{*}(p)$, $q^{*}(p)$.
- [snapshot_producer_test.cpp](../../../cpp/venues/tests/snapshot_producer_test.cpp) — публикация в Kafka venue.snapshots.
- [liquidity_curve_producer_test.cpp](../../../cpp/venues/tests/liquidity_curve_producer_test.cpp) — публикация в Kafka venue.liquidity.fob.
- [venue_adapter_contract_test.cpp](../../../cpp/venues/tests/venue_adapter_contract_test.cpp) — соблюдение интерфейса VenueAdapter всеми реализациями.
- [cex_ws_rest_adapter_test.cpp](../../../cpp/venues/tests/cex_ws_rest_adapter_test.cpp), [cex_local_lob_assembler_test.cpp](../../../cpp/venues/tests/cex_local_lob_assembler_test.cpp), [dex_amm_rpc_adapter_test.cpp](../../../cpp/venues/tests/dex_amm_rpc_adapter_test.cpp), [simulated_venue_adapter_test.cpp](../../../cpp/venues/tests/simulated_venue_adapter_test.cpp), [venue_sim_adapter_test.cpp](../../../cpp/venues/tests/venue_sim_adapter_test.cpp).
- [venue_observability_producer_test.cpp](../../../cpp/venues/tests/venue_observability_producer_test.cpp) — публикация RAW heartbeat'ов.
- [execute_on_venue_test.cpp](../../../cpp/venues/tests/execute_on_venue_test.cpp), [execution_report_producer_test.cpp](../../../cpp/venues/tests/execution_report_producer_test.cpp).
- [venue_state_test.cpp](../../../cpp/venue_health/tests/domain/venue_state_test.cpp), [service_test.cpp](../../../cpp/venue_health/tests/app/service_test.cpp).

## 2. Интеграционные тесты (IN-004 §«Интеграционные тесты»)

| #  | Тест                                                                                 | Файл / Скрипт                                                                                              | Статус |
| -- | ------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------- | ------ |
| I1 | Полный цикл CEX: Raw depth → VenueSnapshot → VenueLiquidityCurve → Matching          | [Testing/f11_test2_e2e.sh](../../../Testing/f11_test2_e2e.sh)                                              | ⚠️ Запускается вручную; CI trigger pending |
| I2 | Полный цикл DEX/AMM: Pool state → virtual LOB / FOB curve → Matching / ExecPlanning | [Testing/f11_dex_mock.py](../../../Testing/f11_dex_mock.py) + [f11_test2_e2e.sh](../../../Testing/f11_test2_e2e.sh) | ⚠️ через mock DEX |
| I3 | Execution hedge: ExecutionIntent → Adapter → EVC → execution.venue → Ledger          | [cpp/venues/tests/execute_on_venue_test.cpp](../../../cpp/venues/tests/execute_on_venue_test.cpp); F-12 e2e | ⚠️ Unit-уровня; полный e2e — F-12 |
| I4 | Disconnect/reconnect cycle                                                           | [cpp/venues/tests/venue_adapter_contract_test.cpp](../../../cpp/venues/tests/venue_adapter_contract_test.cpp) | ✅ |
| I5 | Circuit breaker full state machine                                                   | [cpp/venue_health/tests/app/service_test.cpp](../../../cpp/venue_health/tests/app/service_test.cpp)        | ✅ |
| I6 | Hot reload `venue_config`                                                            | [Testing/f11_test2_e2e.sh](../../../Testing/f11_test2_e2e.sh) (этапы upsert / enable / disable)            | ⚠️ часть e2e |
| I7 | Multi-venue для одного инструмента                                                   | [cpp/venues/tests/backtest_synthetic_scenarios_test.cpp](../../../cpp/venues/tests/backtest_synthetic_scenarios_test.cpp) | ✅ |

Дополнительные:

- [backtest_parity_check_test.cpp](../../../cpp/venues/tests/backtest_parity_check_test.cpp) — backtest-режим даёт идентичный поток исполнений.
- [Testing/f11_test4_quality.sh](../../../Testing/f11_test4_quality.sh) — quality-замер (epsilon1, slippage), косвенно покрывает U8 (L3 calibration).

## 3. Нагрузочные тесты (IN-004 §«Нагрузочные тесты»)

| #  | Сценарий                                                                | Файл                                                                            | Статус                                          |
| -- | ----------------------------------------------------------------------- | ------------------------------------------------------------------------------- | ----------------------------------------------- |
| L1 | 10 одновременных площадок, 20 снапшотов/сек на площадку (200/sec total)  | [Testing/f11_test3_load.sh](../../../Testing/f11_test3_load.sh)                 | ⚠️ Скрипт есть; запускается вручную             |
| L2 | p95 LOB-FOB ≤ 50 ms на инструмент в L1/L2; Kafka lag < 100 сообщений     | [Testing/f11_test3_load.sh](../../../Testing/f11_test3_load.sh) (квантильные замеры) | ⚠️ Метрики собираются; SLA-job pending          |

Метрики из VenueRuntimeMetrics (`last_curve_build_latency_ms`, `snapshots_per_sec`, `stale_rate`) экспортируются через `GET /api/v1/venues` и Observability.

## 4. Ручные тесты

### 4.1. Onboarding test-then-commit

1. Запустить `cd infra && docker compose -f docker-compose.dev.yml up`.
2. `curl -X POST http://localhost:8087/api/v1/venues -H 'Content-Type: application/json' -d '{"venue_id":"test_binance","adapter_mode":"cex_ws_rest","ws_url":"wss://stream.binance.com:9443/ws","rest_base_url":"https://api.binance.com","venue_symbol":"BTCUSDT","curve_level":"L1","is_active":false}'`.
3. `curl -X POST http://localhost:8087/api/v1/venues/test_binance/reconnect`.
4. `curl http://localhost:8087/api/v1/venues/test_binance` — проверить `health.status="connected"`.
5. `curl -X POST http://localhost:8087/api/v1/venues/test_binance/enable`.
6. Подождать 10 сек; `curl http://localhost:8087/api/v1/venues/test_binance/snapshots?limit=5` — должны быть свежие snapshots.

### 4.2. Деградация (stale → circuit breaker)

1. Поднять mock-venue, который шлёт первые 30 секунд и потом замолкает.
2. Дождаться `status=stale` через `STALE_THRESHOLD_MS=15000`.
3. После N errors — наблюдать circuit breaker → OPEN в `GET /api/v1/venues/health`.
4. После `CIRCUIT_BREAKER_COOLDOWN_S` — HALF_OPEN.
5. Восстановить mock; убедиться, что → CLOSED.

### 4.3. LOB → FOB качество

1. Запустить `bash Testing/f11_test4_quality.sh` — генерация синтетических ордеров + проверка `epsilon1 ≤ 1%` на рабочем диапазоне.

### 4.4. Hot reload

1. `PUT /api/v1/venues/{id}` с обновлённым `curve_level=L1`.
2. Без рестарта проверить, что новые curves публикуются с `level=L1`.

## 5. Подключение к task plan и DoD

Каждая позиция [Definition of Done F-11](../../implementation-plan/F-11-external-venues.tasks.md#definition-of-done-in-004) ссылается на конкретные тесты из этого плана.

## Source Fragments

- IN-004 §«Unit-тесты» (12 кейсов)
- IN-004 §«Интеграционные тесты» (7 кейсов)
- IN-004 §«Нагрузочные тесты» (2 сценария)
- IN-004 §«Definition of Done»
