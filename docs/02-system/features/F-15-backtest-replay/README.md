# F-15 — Backtest / Replay

> **Статус:** in-progress, MVP-сервис реализован, UI отсутствует. Самая
> зрелая фича из тройки F-11/F-12/F-15 по покрытию кода.

## 🧭 Navigation Map (IN-013 drill-down)

Эта секция — **карта документации сверху вниз** для фичи.
Каждый уровень имеет свой ответ на «что/как», и каждая ссылка
ведёт на следующий уровень детализации.

```text
   ┌─ Уровень ──────────────┬─ Артефакт ─────────────────────────────────┐
☁️ L0 │ Что система делает    │ Эта страница + L0 system sequence(s) ниже  │
🌊 L1 │ Какие функции у фичи?  │ Use Cases (таблица ниже)                   │
   │ Какие сервисы участвуют?│ L1 service sequences (per-UC)              │
🐟 L2 │ Из каких классов       │ Component overviews + L2 sequences         │
   │ состоит сервис?        │                                            │
💻 src │ Код                    │ cpp/<component>/src/...                    │
   └────────────────────────┴────────────────────────────────────────────┘
```

## 📋 Use Cases (L1 🌊)

| UC | Имя | L0 sequence ☁️ | L1 sequence 🌊 |
| --- | --- | --- | --- |
| [UC-F15-01](../../use-cases/UC-F15-01-create-replay-session/use-case.md) | Create Replay Session | [SEQ-UC-F15-01-system](../../use-cases/UC-F15-01-create-replay-session/sequences/SEQ-UC-F15-01-system.md) | [SEQ-F15-UC-F15-01-services](../../../05-components/sequences/SEQ-F15-UC-F15-01-services.md) |
| [UC-F15-01](../../use-cases/UC-F15-01-replay-historical-batch/use-case.md) | Replay Historical Batch | [SEQ-UC-F15-01-system](../../use-cases/UC-F15-01-replay-historical-batch/sequences/SEQ-UC-F15-01-system.md) | [SEQ-F15-UC-F15-01-services](../../../05-components/sequences/SEQ-F15-UC-F15-01-services.md) |
| [UC-F15-02](../../use-cases/UC-F15-02-cancel-replay-session/use-case.md) | Cancel Replay Session | [SEQ-UC-F15-02-system](../../use-cases/UC-F15-02-cancel-replay-session/sequences/SEQ-UC-F15-02-system.md) | [SEQ-F15-02-replay-cycle-services](../../../05-components/sequences/SEQ-F15-02-replay-cycle-services.md) |
| [UC-F15-03](../../use-cases/UC-F15-03-ab-compare-sessions/use-case.md) | Ab Compare Sessions | [SEQ-UC-F15-03-system](../../use-cases/UC-F15-03-ab-compare-sessions/sequences/SEQ-UC-F15-03-system.md) | [SEQ-F15-03-cancel-services](../../../05-components/sequences/SEQ-F15-03-cancel-services.md) |
| [UC-F15-04](../../use-cases/UC-F15-04-audit-mode-replay/use-case.md) | Audit Mode Replay | [SEQ-UC-F15-04-system](../../use-cases/UC-F15-04-audit-mode-replay/sequences/SEQ-UC-F15-04-system.md) | [SEQ-F15-04-audit-mode-services](../../../05-components/sequences/SEQ-F15-04-audit-mode-services.md) |
| [UC-F15-05](../../use-cases/UC-F15-05-retry-failed-session/use-case.md) | Retry Failed Session | [SEQ-UC-F15-05-system](../../use-cases/UC-F15-05-retry-failed-session/sequences/SEQ-UC-F15-05-system.md) | — |
| [UC-F15-06](../../use-cases/UC-F15-06-replay-determinism-check/use-case.md) | Replay Determinism Check | [SEQ-UC-F15-06-system](../../use-cases/UC-F15-06-replay-determinism-check/sequences/SEQ-UC-F15-06-system.md) | — |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| [backtest-service](../../../05-components/backtest-service/overview.md) | [SEQ-BACKTEST-01-create-session](../../../05-components/backtest-service/sequences/SEQ-BACKTEST-01-create-session.md), [SEQ-BACKTEST-02-batch-replay-step](../../../05-components/backtest-service/sequences/SEQ-BACKTEST-02-batch-replay-step.md) |
| [matching-fob-core](../../../05-components/matching-fob-core/overview.md) | [SEQ-MATCHING-001-solver-cycle](../../../05-components/matching-fob-core/sequences/SEQ-MATCHING-001-solver-cycle.md) |
| [risk-manager](../../../05-components/risk-manager/overview.md) | (L2 sequences pending) |
| `settlement-ledger` (overview pending) | (L2 sequences pending) |
| [market-data](../../../05-components/market-data/overview.md) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

## Бизнес-смысл

Backtest/Replay даёт три ключевых возможности:

1. **Тестирование стратегий и solver-конфигов.** Аналитик может загрузить
   историческое окно, прогнать его через ту же логику что и production
   (Matching + Risk + Ledger), и получить ReplaySummary с PnL, Sharpe,
   FillRate, IS, VWAP, MaxDrawdown.
2. **Audit / Forensic mode.** При спорной сделке оператор может реплееть
   единственный `batch_id` и сравнить с production BatchResult (diff
   `clear_prices`, `executed_rates`, `residual_norm`, `fills`).
3. **RL training / agent log.** Каждый шаг записывается как
   `state-action-reward` в ClickHouse (`replay_agentlogs`), что готовит
   данные для обучения policy.

A/B compare двух сессий с одинаковым date range сравнивает изменения в
solver_config или risk_limits через delta-матрицу
(`avg_is`, `total_pnl`, `sharpe`, `fill_rate`, `max_drawdown`).

## Ключевые сущности

- **ReplaySession** — single replay run (id, user, name, strategy JSON,
  date range, snapshot конфигов, status pending→running→completed/failed/cancelled).
  Хранится в PostgreSQL `replay_sessions`.
- **AgentLog** — per-batch state/action/reward + diagnostics (PnL, IS,
  FillRate, fills, batch_result_json, solver_error_flag, error_code,
  failure_component). Хранится в ClickHouse `replay_agentlogs`.
- **ReplaySummary** — агрегаты по сессии (total_pnl, avg_pnl, std_pnl,
  sharpe, fill_rate, avg_vwap, avg_solve_time, max_drawdown). Хранится в
  PostgreSQL `replay_summaries`.
- **ShadowNamespace** — изолированный namespace ledger'а для replay
  (`shadow:<session_id>` prefix), не задевает production баланс.
- **ConfigSnapshot** — замороженный snapshot solver_config, risk_limits,
  fee_model, reward_config и random_seed, обеспечивающий детерминизм.
- **AuditRun** — single-batch replay в audit mode + diff vs production.
- **Reward mode** — `pnl` (incremental PnL), `-is` (отрицательный IS) или
  `hybrid` (взвешенная сумма).

## Архитектура

```text
                  ┌────────────────────────────────────────────────┐
                  │            cpp/backtest (port 8087)             │
   REST/Kafka     │  ┌──────────────┐   ┌─────────────────────┐    │
   ──────────▶   │  │ CreateReplay │   │  RunReplaySession   │    │
                  │  │ /Cancel/Retry│──▶│  (orchestrator)     │    │
                  │  └──────────────┘   └──────────┬──────────┘    │
                  │                                │               │
                  │            ┌───────────────────┼─────────────┐ │
                  │            ▼                   ▼             ▼ │
                  │   HistoricalBatchLoader   ShadowNamespace  GrpcBatchExecutor
                  │       (ClickHouse)            +Ledger      (Matching, Risk)
                  │            │                   │             │ │
                  │            └─────┬─────────────┴─────┬───────┘ │
                  │                  ▼                   ▼         │
                  │         ReplayStepJournal      AgentLogWriter  │
                  │         (PG summaries)         (ClickHouse)    │
                  │                  │                             │
                  │                  ▼                             │
                  │           ReplayEventPublisher                 │
                  │           (Kafka replay.results)               │
                  └────────────────────────────────────────────────┘
```

Детальные диаграммы:

- System sequence: [UC-F15-01](../../use-cases/UC-F15-01-create-replay-session/sequences/SEQ-UC-F15-01-system.md), [UC-F15-04 audit](../../use-cases/UC-F15-04-audit-mode-replay/sequences/SEQ-UC-F15-04-system.md)
- Service sequences: [SEQ-F15-01 create](../../../05-components/sequences/SEQ-F15-01-create-session-services.md), [SEQ-F15-02 cycle](../../../05-components/sequences/SEQ-F15-02-replay-cycle-services.md), [SEQ-F15-03 cancel](../../../05-components/sequences/SEQ-F15-03-cancel-services.md), [SEQ-F15-04 audit](../../../05-components/sequences/SEQ-F15-04-audit-mode-services.md)
- Internal: [backtest-service overview](../../../05-components/backtest-service/overview.md)

## Реализация

- Service entry: [cpp/backtest/src/main.cpp](../../../../cpp/backtest/src/main.cpp)
- App layer (use cases): [cpp/backtest/src/app/](../../../../cpp/backtest/src/app/) — 19 use cases + ports
- Infra: [cpp/backtest/src/infra/](../../../../cpp/backtest/src/infra/) (ClickHouse, PostgreSQL, Kafka, gRPC, HTTP)
- Migrations: [cpp/backtest/migrations/postgres/](../../../../cpp/backtest/migrations/postgres/) (001 schema, 002 RBAC, 003 retry_parent_id)
- Docker dev: [infra/docker-compose.dev.yml](../../../../infra/docker-compose.dev.yml) service `backtest` (port 8087)

## Acceptance criteria

См. [acceptance-criteria.md](acceptance-criteria.md): функциональные F15-1..F15-47, нефункциональные AC-N1..AC-N9, DoD соответствие.

## Контракты

- REST: [docs/06-api/rest/replay.md](../../../06-api/rest/replay.md) (генерируется из [contracts/openapi/fob/replay/v1/api/replay.yaml](../../../../contracts/openapi/fob/replay/v1/api/replay.yaml))
- Kafka: [docs/06-api/messaging/replay-topics.md](../../../06-api/messaging/replay-topics.md) (`replay.commands`, `replay.results`)
- Proto: [contracts/proto/fob/replay/v1/replay.proto](../../../../contracts/proto/fob/replay/v1/replay.proto)
- gRPC calls: `fob.matching.v1.Solver/Solve`, `fob.risk.v1.RiskService/CheckPostTrade`, `fob.ledger.v1.LedgerService/ApplyFills` (shadow)

## Данные

- PostgreSQL: [replay-sessions](../../../07-data/replay-sessions.md), [replay-summaries](../../../07-data/replay-summaries.md), [replay-rbac](../../../07-data/replay-rbac.md)
- ClickHouse: [replay-agentlogs](../../../07-data/replay-agentlogs.md)

## Доменные формулы и инварианты

См. [04-domain/business-rules.md §F-15](../../../04-domain/business-rules.md#f-15--backtest--replay) — Sharpe, IS, VWAP, FillRate, MaxDD, AgentState и ShadowPositions.

## Связанные фичи

- **F-04 Batch Clearing** — solver вызывается в isolation mode (`fob.matching.v1.Solver/Solve`) тем же кодом, что и production scheduler. F-15 проверяет parity F-04.
- **F-11 External Venues LOB→FOB** — historical venue snapshots/curves используются [VenueReplayUC](../../../../cpp/backtest/src/app/venue_replay_uc.cpp) для парити-проверок и [LobFobReplayUC](../../../../cpp/backtest/src/app/lob_fob_replay_uc.cpp) для quality replay.
- **F-12 Execution Hedge** — VenueSim (часть backtest) воспроизводит ExecutionIntent → ExecutionReport flow без EVC. Это обязательное parity-требование F12-9 (backtest parity).
- **F-06 Positions/PnL** — формулы PnL и MaxDD согласованы с post-trade модулем.
- **F-13 Post-trade reports** — те же IS и VWAP формулы.
- **F-16 Operator panel** — будущая интеграция: запуск audit-mode replay из operator UI.

## Definition of Done (IN-006)

См. [acceptance-criteria.md §4](acceptance-criteria.md#4-definition-of-done--соответствие-in-006-22-пункта). Покрытие 19/22 ✅, 2/22 ⚠, 1/22 ❌ (Web UI).

## Open Questions

См. [open-questions.md](open-questions.md).

## Source Fragments

- IN-006 (детальная спецификация F-15, 47 функциональных + DoD)
- IN-001-FR-027, IN-001-FR-028 (общие требования к backtest/replay из vision)
- Cross-link: IN-003 (F-04 solver parity), IN-004 (F-11 venue parity), IN-005 (F-12 hedge parity)
