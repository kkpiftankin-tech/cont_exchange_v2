---
id: CMP-BACKTEST-SERVICE
phase: 05-components
status: implemented-mvp
component: backtest-service
related:
  - docs/02-system/features/F-15-backtest-replay/
---

# Component: Backtest / Replay Service

> Замечание: эта страница описывает **исполняющий сервис** `cpp/backtest`.
> Существует параллельный заглушечный документ
> [backtest-replay/overview.md](../backtest-replay/overview.md), оставленный
> для traceability. После полной миграции docs его следует поместить в
> redirect-режим.

## Назначение

Backtest/Replay сервис — оркестратор сценариев воспроизведения исторических
торговых данных через тот же код, что и production (matching, risk, ledger),
но в shadow namespace. Используется для:

- backtest стратегий и тюнинга solver_config,
- regression-тестирования parity (F-04, F-11, F-12),
- audit (forensic) разбора отдельных батчей,
- training данных для RL (`state-action-reward` logs).

## Архитектура (слоистая)

```text
transport/        REST (Crow) + Kafka consumers + Kafka producer
  └── /api/v1/replay/*  (port 8087)
app/              use cases (DDD application layer)
  ├── CreateReplaySession  (validation, snapshot, insert pending)
  ├── RunReplaySession     (orchestrator: poll pending → run → terminal)
  ├── CancelReplaySession  (ICancellationToken)
  ├── RetryReplaySession   (use_same_config | override_config)
  ├── ReadReplaySessions   (filters, pagination)
  ├── CompareReplaySessions(A/B + cache)
  ├── CheckReplayDeterminism
  ├── RunReplayAuditBatch  (single batch + diff)
  ├── HistoricalBatchLoader (ClickHouse-only loader)
  ├── ShadowNamespace      (in-memory shadow ledger init)
  ├── RestoreState         (resume after restart)
  ├── BacktestUC           (parity-check stream of production batches)
  ├── LobFobReplayUC       (F-11 curve quality replay — beyond IN-006)
  ├── VenueReplayUC        (F-12 venue parity)
  └── QualityMetricsUC     (epsilon distributions for venue curves)
domain (implicit via shared cpp/common types + reuse cpp/ledger ledger_uc):
  ├── ReplaySession status enum
  ├── ConfigSnapshot
  └── ShadowPositions
infra/            adapters
  ├── PostgresReplaySessionRepository      (replay_sessions, replay_summaries)
  ├── PostgresReplayConfigRepository       (solver_config, risk_limits)
  ├── PostgresRbacRepository               (roles, permissions, audit_log)
  ├── ClickHouseStorage                    (EnsureSchema + INSERT JSONEachRow)
  ├── ClickHouseHistoricalQueries          (read batchresults/fills/marketdata)
  ├── GrpcReplayBatchExecutor              (Solver isolation gRPC client)
  ├── InMemoryShadowLedger                 (per-session namespace, no persistence)
  ├── KafkaReplayEventPublisher            (replay.results producer)
  ├── ReplayCommandsKafkaConsumer          (replay.commands consumer)
  ├── KafkaConsumer (batch.outputs)        (parity stream)
  ├── VenueKafkaConsumer                   (venue.snapshots)
  └── HttpRetrySessionHandler              (Crow handler for retry)
```

## Ключевые модули

| Модуль                          | Файл                                                                                                                                                  | Что делает                                                                                                       |
| ------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| RunReplaySession (orchestrator) | [run_replay_session_uc.cpp](../../../cpp/backtest/src/app/run_replay_session_uc.cpp)                                                                  | End-to-end жизненный цикл сессии: pending → running → completed/failed/cancelled.                                |
| ReplayStepJournal               | [replay_step_journal.cpp](../../../cpp/backtest/src/app/replay_step_journal.cpp)                                                                      | Per-batch агрегация: AgentLog + summary builder (Sharpe, IS, VWAP, FillRate, MaxDD).                              |
| ReplayConfigSnapshotBuilder     | [replay_config_snapshot.cpp](../../../cpp/backtest/src/app/replay_config_snapshot.cpp)                                                                | Замораживает solver_config + risk_limits + fee_model + reward_config + random_seed в JSON.                         |
| ShadowNamespaceInitializer      | [shadow_namespace_uc.cpp](../../../cpp/backtest/src/app/shadow_namespace_uc.cpp)                                                                      | Создаёт изолированный namespace в `InMemoryShadowLedger` с начальными балансами.                                 |
| RbacEngine                      | [rbac_engine.hpp](../../../cpp/backtest/src/app/rbac_engine.hpp)                                                                                      | Авторизация на permission level; вход в UC через `Dependencies.rbac_engine`.                                     |
| HistoricalBatchLoader           | [historical_batch_loader_uc.cpp](../../../cpp/backtest/src/app/historical_batch_loader_uc.cpp)                                                        | Coverage check + потоковая загрузка истории из ClickHouse.                                                       |
| RestoreState                    | [restore_state_uc.cpp](../../../cpp/backtest/src/app/restore_state_uc.cpp)                                                                            | При retry для дублирующих batch_id восстанавливает state без двойного учёта.                                    |

## Shadow Isolation

`InMemoryShadowLedger` хранит балансы и резервы под ключом
`shadow:<session_id>:<account_id>`. Production Collateral Ledger живёт в
отдельном процессе и не задействуется. Тест:
[shadow_ledger_apply_test.cpp](../../../cpp/backtest/tests/shadow_ledger_apply_test.cpp).

Альтернатива (отдельная Postgres schema) обсуждается в [ADR-009](../../03-architecture/adr/ADR-009-shadow-mode-isolation-strategy.md).

## RBAC

Три системные роли с предзаданными permissions:

| Role     | Permissions                                                          |
| -------- | -------------------------------------------------------------------- |
| admin    | `replay:create`, `replay:read`, `replay:execute`, `replay:cancel`, `audit:read` |
| analyst  | `replay:create`, `replay:read`, `replay:execute`, `replay:cancel`    |
| viewer   | `replay:read`                                                        |

Каждое действие пишется в `audit_log` (PostgreSQL) с `user_id`, `status`
(`success`/`failure`/`denied`), `ip_address`, `user_agent`, `correlation_id`.

См. [docs/07-data/replay-rbac.md](../../07-data/replay-rbac.md).

## Связанные фичи

- F-15 — основная фича.
- F-04 — solver вызывается в isolation mode.
- F-11 — quality-replay LOB→FOB через `lob_fob_replay_uc.cpp`.
- F-12 — venue parity через `venue_replay_uc.cpp` (VenueSim replaces EVC).
- F-13 — те же IS / VWAP формулы.

## Participates In Features

- [F-15](../../02-system/features/F-15-backtest-replay/)

## Participates In Use Cases

- [UC-F15-01 Create](../../02-system/use-cases/UC-F15-01-create-replay-session/use-case.md)
- [UC-F15-02 Cancel](../../02-system/use-cases/UC-F15-02-cancel-replay-session/use-case.md)
- [UC-F15-03 Compare](../../02-system/use-cases/UC-F15-03-ab-compare-sessions/use-case.md)
- [UC-F15-04 Audit](../../02-system/use-cases/UC-F15-04-audit-mode-replay/use-case.md)
- [UC-F15-05 Retry](../../02-system/use-cases/UC-F15-05-retry-failed-session/use-case.md)
- [UC-F15-06 Determinism](../../02-system/use-cases/UC-F15-06-replay-determinism-check/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F15-01-create-session-services](../sequences/SEQ-F15-01-create-session-services.md)
- [SEQ-F15-02-replay-cycle-services](../sequences/SEQ-F15-02-replay-cycle-services.md)
- [SEQ-F15-03-cancel-services](../sequences/SEQ-F15-03-cancel-services.md)
- [SEQ-F15-04-audit-mode-services](../sequences/SEQ-F15-04-audit-mode-services.md)
- Internal: [SEQ-BACKTEST-01-create-session](sequences/SEQ-BACKTEST-01-create-session.md), [SEQ-BACKTEST-02-batch-replay-step](sequences/SEQ-BACKTEST-02-batch-replay-step.md)

## Owned Contracts

- REST: [docs/06-api/rest/replay.md](../../06-api/rest/replay.md)
- Proto: [contracts/proto/fob/replay/v1/replay.proto](../../../contracts/proto/fob/replay/v1/replay.proto)
- Kafka topics: `replay.commands`, `replay.results`

## Produced Events

- `replay.results` (`ReplayProgressEvent`, `ReplayCompletedEvent`, `ReplayFailedEvent`, `ReplayCancelledEvent`)

## Consumed Events

- `replay.commands` (`CreateReplayCommand`, `CancelReplayCommand`, `RetryReplayCommand`) — опционально, REST альтернатива
- `batch.outputs` (read-only, parity-checks)
- `venue.snapshots` (read-only, F-11 parity)

## Data Access

- write: `replay_sessions`, `replay_summaries`, `replay_compare_cache`, `replay_audit_runs` (PostgreSQL); `replay_agentlogs` (ClickHouse); RBAC tables (PostgreSQL).
- read-only: `batchresults`, `fills`, `marketdata_snapshots`, `venue_snapshots` (ClickHouse); `solver_config`, `risk_limits` (PostgreSQL).

## Configuration

Полный список env vars — в [component.yaml](component.yaml#config).

## Статус

Implemented MVP. Полное покрытие unit + integration + load тестами.
UI не реализован.

## Source Fragments

- IN-006-FR-001..047
- Cross-link: IN-003 (F-04), IN-004 (F-11), IN-005 (F-12)
