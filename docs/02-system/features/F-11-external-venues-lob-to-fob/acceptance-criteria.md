# Acceptance Criteria — F-11

Источник: [IN-004](../../../../incoming-docs/2026-05-20-F-11-external-venues-v1.md) §«Definition of Done», §«Функциональные требования F11-1..F11-20», §«Нефункциональные требования».

## Функциональные

| #     | Критерий                                                                                          | Статус                                                                                                                                       |
| ----- | ------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| AC-1  | External Venues Connector подключается к ≥ 2 CEX и ≥ 1 DEX/AMM (F11-1, F11-2)                     | ✅ Адаптеры: Binance (ws+rest), Coinbase (ws+rest), Uniswap v3 (rpc) — см. [cex_ws_rest_adapter.cpp](../../../../cpp/venues/src/infra/cex_ws_rest_adapter.cpp), [dex_amm_rpc_adapter.cpp](../../../../cpp/venues/src/infra/dex_amm_rpc_adapter.cpp) |
| AC-2  | Каждое событие преобразуется в VenueSnapshot (F11-3)                                              | ✅ [normalize_snapshot.cpp](../../../../cpp/venues/src/domain/normalize_snapshot.cpp); тест [normalize_snapshot_test.cpp](../../../../cpp/venues/tests/normalize_snapshot_test.cpp) |
| AC-3  | VenueSnapshot публикуется в Kafka venue.snapshots (F11-4)                                         | ✅ [snapshot_producer.cpp](../../../../cpp/venues/src/app/snapshot_producer.cpp) — `snap_cfg.topic = "venue.snapshots"` (venues_loop.cpp:375)  |
| AC-4  | VenueSnapshot пишется в ClickHouse с retention ≥ 90 дней                                          | ⚠️ Writer [snapshot_clickhouse_writer.cpp](../../../../cpp/venues/src/infra/snapshot_clickhouse_writer.cpp) реализован; DDL `venue_snapshots` отсутствует в [infra/clickhouse/init.sql](../../../../infra/clickhouse/init.sql) — отдельная задача T-F11-110 |
| AC-5  | Venue Liquidity Curve Builder — отдельный компонент (F11-5)                                        | ⚠️ Реализован как [LiquidityCurveProducer](../../../../cpp/venues/src/app/liquidity_curve_producer.cpp) внутри `cpp/venues` (одно бинарь). Спецификация описывает как отдельный компонент. Conflict Note C-1 |
| AC-6  | Поддержка L1/L2/L3 режимов (F11-6)                                                                | ✅ `CURVE_LEVEL` env: L1/L2/L3; [depth_curve_builder.cpp](../../../../cpp/venues/src/domain/depth_curve_builder.cpp), [depth_curve_regularization_test.cpp](../../../../cpp/venues/tests/depth_curve_regularization_test.cpp) |
| AC-7  | L1: монотонная аппроксимация p(q), S(q), L(v) (F11-7)                                              | ✅ `l_of_v_monotone` в proto + [depth_curve_builder.cpp](../../../../cpp/venues/src/domain/depth_curve_builder.cpp); тест [depth_curve_builder_test.cpp](../../../../cpp/venues/tests/depth_curve_builder_test.cpp) |
| AC-8  | L2: выпуклость + Moreau/Tikhonov + epsilon1, epsilon2 (F11-8)                                      | ✅ [depth_curve_regularization_test.cpp](../../../../cpp/venues/tests/depth_curve_regularization_test.cpp); epsilon1/epsilon2 в VenueLiquidityCurve proto |
| AC-9  | L3: калибровка по execution reports + epsilon3 (F11-9)                                             | ⚠️ Поле `epsilon3` в proto; полная калибровочная петля (consume execution reports → обновление модели) — частично, источник execution reports не зафиксирован (см. open-questions.md §2) |
| AC-10 | VenueLiquidityCurve публикуется в venue.liquidity.fob (F11-10)                                     | ✅ [liquidity_curve_producer.hpp](../../../../cpp/venues/src/app/liquidity_curve_producer.hpp#L140) — `topic{"venue.liquidity.fob"}`             |
| AC-11 | Производное построение SyntheticFlowOrder (F11-11)                                                 | ✅ `curve_cfg.synthetic.topic` (venues_loop.cpp:523-524); [postgres_synthetic_order_repository.cpp](../../../../cpp/venues/src/infra/postgres_synthetic_order_repository.cpp) — lifecycle (active/expired/used) |
| AC-12 | Matching Backend использует внешнюю ликвидность в FOB-форме (F11-12)                               | ⚠️ Matching импортирует `venue.liquidity.fob`/`venue.synthetic` (см. [external_venue_filter_test.cpp](../../../../cpp/matching/tests/app/external_venue_filter_test.cpp)); полная интеграция в основной solver pipeline — F-09 + F-04 cross-link |
| AC-13 | Risk Manager учитывает venue.health (F11-13)                                                       | ❌ Risk Manager пока не консьюмит venue.health; задача T-F11-200                                                                              |
| AC-14 | Execution Planning использует VenueLiquidityCurve и venue.health (F11-14)                          | ⚠️ Частично — [planner_inputs_cache_test.cpp](../../../../cpp/matching/tests/app/planner_inputs_cache_test.cpp) уже знает про venue.liquidity.fob; routing-decision использует venue.health через cpp/venues runtime (`routing_recommendation`) |
| AC-15 | Venue Execution Adapter: Intent → child orders → execution.venue (F11-15)                          | ✅ [execute_on_venue.cpp](../../../../cpp/venues/src/app/execute_on_venue.cpp); [execution_intents_consumer.cpp](../../../../cpp/venues/src/infra/execution_intents_consumer.cpp); тест [execute_on_venue_test.cpp](../../../../cpp/venues/tests/execute_on_venue_test.cpp). Бизнес-логика хеджирования — F-12 |
| AC-16 | Stale detection при `STALE_THRESHOLD_MS` → пауза FOB (F11-16)                                      | ✅ [snapshot_status.cpp](../../../../cpp/venues/src/domain/snapshot_status.cpp); тест [snapshot_status_test.cpp](../../../../cpp/venues/tests/snapshot_status_test.cpp) |
| AC-17 | Circuit Breaker CLOSED/OPEN/HALF_OPEN per-venue (F11-17)                                           | ✅ [circuit_breaker.cpp](../../../../cpp/venue_health/src/domain/entities/circuit_breaker.cpp); тест [circuit_breaker_test.cpp](../../../../cpp/venue_health/tests/domain/circuit_breaker_test.cpp) |
| AC-18 | DEX/AMM virtual LOB (F11-18)                                                                       | ✅ [amm_pool_extractor.cpp](../../../../cpp/venues/src/domain/amm_pool_extractor.cpp), [amm_virtual_lob.cpp](../../../../cpp/venues/src/domain/amm_virtual_lob.cpp); тесты [amm_pool_extractor_test.cpp](../../../../cpp/venues/tests/amm_pool_extractor_test.cpp), [amm_virtual_lob_test.cpp](../../../../cpp/venues/tests/amm_virtual_lob_test.cpp) |
| AC-19 | Admin API CRUD venue_config + hot reload (F11-19)                                                  | ✅ Crow HTTP в [main.cpp](../../../../cpp/venues/src/main.cpp) (порт `VENUES_ADMIN_HTTP_PORT=8087`); persist в [postgres_venue_config_repository.cpp](../../../../cpp/venues/src/infra/postgres_venue_config_repository.cpp) |
| AC-20 | Логирование версий моделей LOB-FOB для backtest/replay (F11-20)                                    | ✅ `schema_version`, `min_compatible_schema_version`, `producer_version` в proto `VenueLiquidityCurve`; backtest parity в [backtest_parity_check.cpp](../../../../cpp/venues/src/app/backtest_parity_check.cpp) |

## Нефункциональные

| #     | Критерий                                                                                | Статус                                                                                          |
| ----- | --------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| AC-21 | p95 latency получения VenueSnapshot < 500 ms                                            | ⚠️ Метрика `venue.snapshots_per_sec` в [venue_observability_producer.cpp](../../../../cpp/venues/src/infra/venue_observability_producer.cpp); нагрузочный замер — [Testing/f11_test3_load.sh](../../../../Testing/f11_test3_load.sh) |
| AC-22 | Throughput ≥ 100 VenueSnapshot/sec суммарно (≥ 200 в нагрузочных тестах)                | ⚠️ См. [Testing/f11_test3_load.sh](../../../../Testing/f11_test3_load.sh) target 200/sec; SLA-job не настроен |
| AC-23 | p95 LOB→FOB конверсии < 50 ms на инструмент (L1/L2)                                    | ⚠️ Метрика `last_curve_build_latency_ms` в VenueRuntimeMetrics; SLA-замер pending                |
| AC-24 | venue.health публикуется ≤ 1 секунды после инцидента                                   | ✅ Service::OnRawReport → Publish immediately ([service.cpp](../../../../cpp/venue_health/src/app/service.cpp))                              |
| AC-25 | Cost approximation error epsilon1 ≤ 1% на рабочем диапазоне объёмов                    | ⚠️ Метрика `epsilon1` рассчитывается; формальный assertion — в [Testing/f11_test4_quality.sh](../../../../Testing/f11_test4_quality.sh) |
| AC-26 | История VenueSnapshot и VenueLiquidityCurve в ClickHouse ≥ 90 дней                     | ⚠️ Параметр `VENUES_CLICKHOUSE_RETENTION_DAYS=90` в writer; полная DDL отсутствует — T-F11-110   |
| AC-27 | Hot reload venue_config без перезапуска                                                  | ✅ `VenuesLoop::UpsertVenueConfig` + `apply_runtime_config_locked` ([venues_loop.cpp](../../../../cpp/venues/src/app/venues_loop.cpp))           |
| AC-28 | Graceful degradation L3 → L2 → L1 → OFF                                                 | ✅ `curve_cfg.degradation.publish_stale_l1_fallback` + `last_curve_degradation_reason` метрика    |

## Unit-тесты (из спецификации IN-004 §«Тестовые кейсы»)

| #   | Тест                                                                  | Файл                                                                                                       | Статус |
| --- | --------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- | ------ |
| U1  | Нормализация простого стакана CEX (5 bid + 5 ask)                     | [normalize_snapshot_test.cpp](../../../../cpp/venues/tests/normalize_snapshot_test.cpp)                    | ✅     |
| U2  | Нормализация пустого стакана → status=empty                           | [snapshot_status_test.cpp](../../../../cpp/venues/tests/snapshot_status_test.cpp)                          | ✅     |
| U3  | Нормализация DEX-состояния (sqrtPriceX96, tick, liquidity Uniswap v3) | [amm_pool_extractor_test.cpp](../../../../cpp/venues/tests/amm_pool_extractor_test.cpp)                    | ✅     |
| U4  | LOB-FOB Level 1 для симметричного стакана                              | [depth_curve_builder_test.cpp](../../../../cpp/venues/tests/depth_curve_builder_test.cpp)                  | ✅     |
| U5  | LOB-FOB с учётом комиссий                                             | [depth_curve_builder_test.cpp](../../../../cpp/venues/tests/depth_curve_builder_test.cpp) (fees-fixture)   | ⚠️ Проверить что fee-учет покрыт |
| U6  | LOB-FOB с минимальным объёмом (фильтрация пылевых уровней)             | [depth_canonicalizer_test.cpp](../../../../cpp/venues/tests/depth_canonicalizer_test.cpp)                  | ✅     |
| U7  | Regularized L2 (выпуклость, монотонность, epsilon2=0)                  | [depth_curve_regularization_test.cpp](../../../../cpp/venues/tests/depth_curve_regularization_test.cpp)    | ✅     |
| U8  | Calibrated L3 (execution reports, epsilon3)                            | —                                                                                                          | ❌ Не покрыт unit-тестом; интеграционный сценарий в [Testing/f11_test4_quality.sh](../../../../Testing/f11_test4_quality.sh) |
| U9  | DEX/AMM virtual LOB                                                    | [amm_virtual_lob_test.cpp](../../../../cpp/venues/tests/amm_virtual_lob_test.cpp)                          | ✅     |
| U10 | Circuit breaker: CLOSED → OPEN                                         | [circuit_breaker_test.cpp](../../../../cpp/venue_health/tests/domain/circuit_breaker_test.cpp)             | ✅     |
| U11 | Circuit breaker: OPEN → HALF_OPEN → CLOSED                             | [circuit_breaker_test.cpp](../../../../cpp/venue_health/tests/domain/circuit_breaker_test.cpp)             | ✅     |
| U12 | Stale detection (пересечение порога свежести)                          | [snapshot_status_test.cpp](../../../../cpp/venues/tests/snapshot_status_test.cpp)                          | ✅     |

## Интеграционные тесты (из спецификации IN-004 §«Интеграционные тесты»)

| #  | Тест                                                                        | Файл / Скрипт                                                                                             | Статус |
| -- | --------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- | ------ |
| I1 | Полный цикл CEX: Raw depth → VenueSnapshot → VenueLiquidityCurve → Matching | [Testing/f11_test2_e2e.sh](../../../../Testing/f11_test2_e2e.sh)                                          | ⚠️ Скрипт есть; запускается вручную; CI-trigger pending |
| I2 | Полный цикл DEX/AMM: Pool state → virtual LOB / FOB curve → Matching/ExecPlan | [Testing/f11_dex_mock.py](../../../../Testing/f11_dex_mock.py) + [Testing/f11_test2_e2e.sh](../../../../Testing/f11_test2_e2e.sh) | ⚠️ Mock есть; e2e через мок-DEX |
| I3 | Execution hedge: ExecutionIntent → Adapter → EVC → execution.venue → Ledger   | [execute_on_venue_test.cpp](../../../../cpp/venues/tests/execute_on_venue_test.cpp); F-12 cross-link       | ⚠️ Unit-уровня; полный e2e — F-12  |
| I4 | Disconnect/reconnect cycle                                                  | [venue_adapter_contract_test.cpp](../../../../cpp/venues/tests/venue_adapter_contract_test.cpp)            | ✅     |
| I5 | Circuit breaker full state machine                                          | [service_test.cpp](../../../../cpp/venue_health/tests/app/service_test.cpp)                                | ✅     |
| I6 | Hot reload venue_config                                                     | [Testing/f11_test2_e2e.sh](../../../../Testing/f11_test2_e2e.sh) (этапы upsert/disable/enable)             | ⚠️ Часть e2e |
| I7 | Multi-venue для одного инструмента                                          | [backtest_synthetic_scenarios_test.cpp](../../../../cpp/venues/tests/backtest_synthetic_scenarios_test.cpp) | ✅     |

## Нагрузочные тесты (из IN-004 §«Нагрузочные тесты»)

| #  | Сценарий                                                              | Файл                                                                          | Статус                                         |
| -- | --------------------------------------------------------------------- | ----------------------------------------------------------------------------- | ---------------------------------------------- |
| L1 | 10 одновременных площадок, 20 снапшотов/сек на площадку (200/sec total) | [Testing/f11_test3_load.sh](../../../../Testing/f11_test3_load.sh)            | ⚠️ Скрипт есть; запускается вручную            |
| L2 | p95 LOB-FOB ≤ 50 ms на инструмент в L1/L2; Kafka lag < 100 сообщений   | [Testing/f11_test3_load.sh](../../../../Testing/f11_test3_load.sh)            | ⚠️ Метрики собираются; SLA-job pending         |

> Легенда: ✅ выполнено, ⚠️ частично, ❌ не выполнено.
