---
id: DOC-TEST-F-15
phase: 10-testing
status: implemented-mvp
owner: core-team
source:
  - IN-006 § Тестовые кейсы
related:
  - docs/02-system/features/F-15-backtest-replay/
  - docs/implementation-plan/F-15-backtest-replay.tasks.md
---

# F-15 Backtest / Replay — план тестирования

Полный спецификационный источник: [`incoming-docs/2026-05-20-F-15-backtest-replay-v1.md`](../../../incoming-docs/2026-05-20-F-15-backtest-replay-v1.md) § Тестовые кейсы.

> Легенда: ✅ выполнено, ⚠️ частично, ❌ не выполнено.

## 1. Unit-тесты (16 сценариев)

Тестируется чистая логика метрик, snapshot, RBAC, AgentLog без полного
стека (без живых Kafka/ClickHouse/Postgres — используются in-memory ports).

| #     | Сценарий                                                                            | Файл                                                                                                                                                                                  | Статус |
| ----- | ----------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ |
| UT-1  | Sharpe ratio (нормальный кейс + std=0 → 0)                                          | [replay_summary_aggregation_test.cpp](../../../cpp/backtest/tests/replay_summary_aggregation_test.cpp)                                                                                | ✅     |
| UT-2  | FillRate (нормальный + sum(Q_max)=0 → 0, `no_requested_volume=true`)                | replay_summary_aggregation_test.cpp                                                                                                                                                    | ✅     |
| UT-3  | Max Drawdown                                                                         | replay_summary_aggregation_test.cpp                                                                                                                                                    | ✅     |
| UT-4  | IS — простая форма + volume-weighted для buy/sell                                    | [metrics_test.cpp](../../../cpp/backtest/tests/metrics_test.cpp), replay_summary_aggregation_test.cpp                                                                                  | ✅     |
| UT-5  | VWAP                                                                                 | metrics_test.cpp                                                                                                                                                                       | ✅     |
| UT-6  | AgentLog формирование (state/action/reward JSON)                                     | [agent_log_writer_test.cpp](../../../cpp/backtest/tests/agent_log_writer_test.cpp), [replay_step_journal_test.cpp](../../../cpp/backtest/tests/replay_step_journal_test.cpp)            | ✅     |
| UT-7  | Детерминизм батча (idempotent ReplacingMergeTree, тот же seed → тот же reward)       | [check_replay_determinism_test.cpp](../../../cpp/backtest/tests/check_replay_determinism_test.cpp)                                                                                     | ✅     |
| UT-8  | Пустой батч (no active flow_orders) — AgentLog с `fills_applied=0`, reward=0          | replay_step_journal_test.cpp                                                                                                                                                            | ✅     |
| UT-9  | Solver не сошёлся (residual_norm > tolerance) — AgentLog `solver_error_flag=1`        | [run_replay_session_test.cpp](../../../cpp/backtest/tests/run_replay_session_test.cpp), replay_step_journal_test.cpp                                                                   | ✅     |
| UT-10 | Boundary формулы (negative pnl, zero volume, single batch session)                   | replay_summary_aggregation_test.cpp                                                                                                                                                     | ✅     |
| UT-11 | Reward modes (pnl, -is, hybrid) — каждый режим даёт ожидаемое значение                | run_replay_session_test.cpp                                                                                                                                                             | ✅     |
| UT-12 | Volume-weighted avgIS (3+ батча с разными exec_qty)                                  | replay_summary_aggregation_test.cpp                                                                                                                                                    | ✅     |
| UT-13 | Агрегация ReplaySummary с ошибочными батчами (processed_batches vs failed_batches)   | replay_summary_aggregation_test.cpp                                                                                                                                                    | ✅     |
| UT-14 | Корректность shadow-позиций (apply fill → пересчёт qty/avg_price; namespace isolation) | [shadow_namespace_test.cpp](../../../cpp/backtest/tests/shadow_namespace_test.cpp), [shadow_ledger_apply_test.cpp](../../../cpp/backtest/tests/shadow_ledger_apply_test.cpp)            | ✅     |
| UT-15 | Сериализация AgentLog (JSONEachRow valid для ClickHouse + round-trip)                | agent_log_writer_test.cpp + [clickhouse_storage_test.cpp](../../../cpp/backtest/tests/clickhouse_storage_test.cpp)                                                                     | ✅     |
| UT-16 | Отсутствие marketdata за период (loader.empty → CreateReplaySession отклоняет)        | [historical_batch_loader_test.cpp](../../../cpp/backtest/tests/historical_batch_loader_test.cpp), [create_replay_session_test.cpp](../../../cpp/backtest/tests/create_replay_session_test.cpp) | ✅     |

Дополнительные unit-тесты вне 16-сценарного MVP плана:

| #     | Сценарий                                                                  | Файл                                                                                                                                                                              | Статус |
| ----- | ------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ |
| UT-17 | ReplayConfigSnapshotBuilder — фиксация всех snapshot полей                 | [replay_config_snapshot_test.cpp](../../../cpp/backtest/tests/replay_config_snapshot_test.cpp)                                                                                    | ✅     |
| UT-18 | ReadReplaySessions — фильтры + пагинация                                   | [read_replay_sessions_test.cpp](../../../cpp/backtest/tests/read_replay_sessions_test.cpp)                                                                                        | ✅     |
| UT-19 | CompareReplaySessions — direction-aware better + cache                     | [compare_replay_sessions_test.cpp](../../../cpp/backtest/tests/compare_replay_sessions_test.cpp)                                                                                  | ✅     |
| UT-20 | CancelReplaySession — pending/running/terminal paths                       | [cancel_replay_session_test.cpp](../../../cpp/backtest/tests/cancel_replay_session_test.cpp)                                                                                       | ✅     |
| UT-21 | RetryReplaySession — strict + override + retry_parent_id propagation       | [app/retry_replay_session_uc_test.cpp](../../../cpp/backtest/tests/app/retry_replay_session_uc_test.cpp)                                                                          | ✅     |
| UT-22 | RestoreState — resume на дублированном batch_id                            | [restore_state_test.cpp](../../../cpp/backtest/tests/restore_state_test.cpp)                                                                                                     | ✅     |
| UT-23 | RunReplayAuditBatch — equivalent / diff / failure                          | [run_replay_audit_batch_test.cpp](../../../cpp/backtest/tests/run_replay_audit_batch_test.cpp)                                                                                   | ✅     |
| UT-24 | KafkaReplayEventPublisher — Progress/Completed/Failed/Cancelled            | [kafka_replay_event_publisher_test.cpp](../../../cpp/backtest/tests/kafka_replay_event_publisher_test.cpp)                                                                       | ✅     |
| UT-25 | GrpcReplayBatchExecutor — Solver/Risk вызовы + error propagation           | [grpc_replay_batch_executor_test.cpp](../../../cpp/backtest/tests/grpc_replay_batch_executor_test.cpp)                                                                            | ✅     |
| UT-26 | LobFobReplayUC — quality replay LOB→FOB curves (F-11 parity)                | [lob_fob_replay_test.cpp](../../../cpp/backtest/tests/lob_fob_replay_test.cpp)                                                                                                   | ✅     |
| UT-27 | VenueReplayUC — F-12 venue parity                                          | [venue_replay_uc_test.cpp](../../../cpp/backtest/tests/venue_replay_uc_test.cpp)                                                                                                 | ✅     |
| UT-28 | QualityMetricsUC — epsilon distributions                                    | [quality_metrics_test.cpp](../../../cpp/backtest/tests/quality_metrics_test.cpp)                                                                                                | ✅     |
| UT-29 | ClickHouseHistoricalQueries — read paths                                    | [clickhouse_historical_queries_test.cpp](../../../cpp/backtest/tests/clickhouse_historical_queries_test.cpp)                                                                     | ✅     |
| UT-30 | ReplayRuntimeMetrics — Prometheus метрики                                   | [replay_runtime_metrics_test.cpp](../../../cpp/backtest/tests/replay_runtime_metrics_test.cpp)                                                                                    | ✅     |
| UT-31 | BacktestUC parity stream from batch.outputs                                 | [backtest_uc_test.cpp](../../../cpp/backtest/tests/backtest_uc_test.cpp)                                                                                                          | ✅     |
| UT-32 | CreateReplaySession — full lifecycle (validation, snapshot, RBAC, audit)    | [create_replay_session_test.cpp](../../../cpp/backtest/tests/create_replay_session_test.cpp)                                                                                     | ✅     |
| UT-33 | RunReplaySession — end-to-end orchestration with mocks                      | [run_replay_session_test.cpp](../../../cpp/backtest/tests/run_replay_session_test.cpp)                                                                                            | ✅     |

## 2. Integration-тесты (10 сценариев)

Поднимаются реальные PostgreSQL / ClickHouse / Kafka (через docker compose);
gRPC моки или живой `matching`.

| #     | Сценарий                                                                                                            | Файл                                                                                                                                                                                                       | Статус |
| ----- | ------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ |
| IT-1  | Полный цикл: POST /sessions → status=pending → running → completed; AgentLog в CH; summary в PG                       | [Testing/f15_test2_e2e.sh](../../../Testing/f15_test2_e2e.sh) + run_replay_session_test.cpp                                                                                                                  | ✅     |
| IT-2  | Shadow isolation: production Collateral Ledger не задет операциями replay                                            | shadow_ledger_apply_test.cpp (verifies namespace separation)                                                                                                                                                | ✅     |
| IT-3  | Kafka progress events: подписавшийся consumer видит ReplayProgress/Completed                                          | kafka_replay_event_publisher_test.cpp + Testing/f15_test2_e2e.sh                                                                                                                                            | ✅     |
| IT-4  | Cancel running mid-flight → status=cancelled, partial summary, partial AgentLog                                       | Testing/f15_test2_e2e.sh §cancel-test; cancel_replay_session_test.cpp                                                                                                                                       | ✅     |
| IT-5  | A/B compare двух completed сессий → корректные delta, cache повторно используется                                    | compare_replay_sessions_test.cpp                                                                                                                                                                            | ✅     |
| IT-6  | Audit-mode для существующего batch_id → equivalent=true в пределах tolerance                                          | run_replay_audit_batch_test.cpp                                                                                                                                                                              | ✅     |
| IT-7  | Неполные исторические данные → graceful failure (status=failed, error_details), partial summary при partial data     | historical_batch_loader_test.cpp + run_replay_session_test.cpp (degraded path)                                                                                                                              | ✅     |
| IT-8  | Retry с use_same_config=true → identical AgentLog и Summary; check_replay_determinism подтверждает                    | retry_replay_session_uc_test.cpp + check_replay_determinism_test.cpp                                                                                                                                        | ✅     |
| IT-9  | Авторизация: viewer не может create/cancel/retry; analyst может только свои; admin все                                | run_replay_session_test.cpp + [postgres_replay_session_repository_test.cpp](../../../cpp/backtest/tests/infra/postgres_replay_session_repository_test.cpp)                                                  | ✅     |
| IT-10 | Большой объём fills (10k+) в одном батче — AgentLog консистентен, нет потерь                                          | [f15_load_harness.cpp](../../../cpp/backtest/tests/f15_load_harness.cpp)                                                                                                                                    | ✅     |

## 3. Нагрузочные тесты

| #     | Сценарий                                            | Файл / запуск                                                                                                                | Порог                  | Статус |
| ----- | --------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- | ---------------------- | ------ |
| LT-1  | 1 000 батчей one session                            | [f15_load_harness.cpp](../../../cpp/backtest/tests/f15_load_harness.cpp) `--mode=1k`; [Testing/f15_perf_slo.py](../../../Testing/f15_perf_slo.py) | wall clock < 60 сек    | ✅     |
| LT-2  | 10 000 батчей one session                           | f15_load_harness.cpp `--mode=10k`                                                                                              | wall clock < 10 мин    | ✅     |
| LT-3  | 5 параллельных сессий                               | f15_load_harness.cpp `--mode=parallel5`                                                                                        | solve degradation < 20% | ✅     |
| LT-4  | 20 параллельных stress                              | f15_load_harness.cpp `--mode=stress20`                                                                                         | p95 ≤ 2× single        | ⚠     |
| LT-5  | AgentLog throughput                                 | f15_load_harness.cpp `--mode=agentlog_throughput`                                                                              | ≥ 200 запис/сек        | ✅     |

`Testing/f15_perf_slo.py` запускается против staging gateway; не сидит на
синтетических данных, требует существующий период в ClickHouse.

## 4. Ручные / диагностические тесты

### 4.1. Функциональный сценарий (UI smoke)

1. Залогиниться как `analyst`.
2. POST /sessions с реальным историческим окном (например, неделя BTC/USDT).
3. Дождаться `status=completed` (poll GET /sessions/{id} или WebSocket).
4. GET /sessions/{id}/summary — проверить разумные значения метрик.
5. GET /sessions/{id}/agentlogs?page=1 — таблица заполнена.
6. Создать вторую сессию с другим `solver_config_id`.
7. GET /compare?sessionA&sessionB — корректная delta-матрица.

### 4.2. Audit-mode smoke

1. Войти как `admin`.
2. Найти подозрительный `batch_id` в production `batchresults`.
3. POST /audit-runs с этим `batch_id` и default tolerance.
4. Проверить `equivalent=true` или drill-down по `diff`.

### 4.3. Failure injection

1. Запустить replay с `tolerance=1e-12` и сложным набором FlowOrder.
2. Ожидать `solver_error_flag=1` в нескольких AgentLog.
3. Проверить, что summary `failed_batches > 0`, replay не падает в `failed`.
4. Остановить ClickHouse mid-session → ожидать `failed` с partial summary.

### 4.4. RBAC smoke

1. Залогиниться как `viewer`.
2. POST /sessions → 403.
3. DELETE /sessions/{id} → 403.
4. GET /sessions/{id чужой} → 403 (ownership).
5. GET /sessions/{id свой} → 200.
6. `audit_log` показывает все `denied` события.

## 5. Подключение к task plan и DoD

- DoD: [features/F-15-backtest-replay/acceptance-criteria.md §4](../../02-system/features/F-15-backtest-replay/acceptance-criteria.md#4-definition-of-done--соответствие-in-006-22-пункта).
- Tasks: [implementation-plan/F-15-backtest-replay.tasks.md](../../implementation-plan/F-15-backtest-replay.tasks.md).

## Source Fragments

- IN-006 § Тестовые кейсы (Unit, Integration, Нагрузочные)
- IN-006 § Definition of Done
