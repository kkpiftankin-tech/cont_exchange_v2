# Открытые вопросы — F-15

## 1. Reward modes — выбор политики по умолчанию

Источник IN-006 фиксирует три режима: `pnl`, `-is`, `hybrid`. Текущая
реализация (`run_replay_session_uc.cpp::ComputeReward`) использует `pnl`
как default, режим хранится в snapshot. Открытые вопросы:

- Какой режим должен быть default для RL training?
- Должен ли `hybrid` иметь конфигурируемые веса PnL и -IS?
- Нужен ли четвёртый режим — risk-adjusted (Sharpe-like)?

## 2. Replay isolation: namespace vs schema

Текущая реализация изолирует replay-позиции через namespace prefix в
in-memory ledger (`shadow:<session_id>:account_id`). Альтернатива —
отдельная PostgreSQL schema для каждой сессии. Вопросы:

- При 5+ параллельных сессий с большими портфелями: хватит ли in-memory?
- Нужна ли persistence для shadow-ledger между retry?
- Должен ли restore_state восстанавливать shadow ledger после crash?

Кандидат на ADR — см. [ADR-009](../../../03-architecture/adr/ADR-009-shadow-mode-isolation-strategy.md).

## 3. RBAC granularity

В коде сейчас три системные роли (admin/analyst/viewer) и 5 permissions
(`replay:create`, `replay:read`, `replay:execute`, `replay:cancel`,
`audit:read`). Вопросы:

- Нужны ли per-session ACL (так что аналитик A не видит сессии аналитика B)?
- Должен ли быть tenant-level isolation (для multi-tenancy)?
- Нужно ли отдельное право на audit-mode (replay чужих batch_id)?

## 4. AgentLog vs ReplayAgentLog — единый источник истины

В проекте есть архитектурное напряжение между:

- `agent_logs` (legacy, упоминается в IN-001 и в [04-domain/entities.md](../../../04-domain/entities.md))
- `replay_agentlogs` (текущая F-15 реализация в ClickHouse)

Должны ли они слиться? Должен ли production matching писать в тот же
формат для unified analytics?

Кандидат на ADR — см. [ADR-010](../../../03-architecture/adr/ADR-010-agentlog-replay-agentlog-unification.md).

## 5. Determinism guarantees

Текущий код фиксирует `random_seed` в snapshot, но F-04 solver (Eigen
Sparse Cholesky) сам по себе deterministic только при стабильном порядке
входов. Вопросы:

- Гарантируется ли deterministic порядок при чтении из ClickHouse?
- Что делать с floating-point reproducibility между разными
  процессорами / Eigen versions?
- Нужен ли byte-exact check в `check_replay_determinism_uc` или
  достаточно tolerance-based?

## 6. UI и WebSocket bridge

В DoD есть пункт «прогресс через WebSocket». Сейчас backtest публикует
`ReplayProgressEvent` в `replay.results`, но WebSocket bridge на стороне
gateway не реализован. Вопросы:

- Должен ли gateway сам подписываться на topic или предоставлять
  long-polling endpoint?
- Нужна ли отдельная WebSocket-схема в `docs/06-api/`?

## 7. ClickHouse retention vs auditability

Retention `replay_agentlogs` = 90 дней (default). Для аудита и
регуляторных проверок может понадобиться год+. Вопросы:

- Должна ли быть выгрузка в S3/Glacier при истечении TTL?
- Нужен ли отдельный долгосрочный архив для audit_runs?

## 8. lob_fob_replay vs feature scope

`cpp/backtest/src/app/lob_fob_replay_uc.cpp` реализует quality-replay
LOB→FOB curve reconstruction. Это feature beyond IN-006 (F-11 parity).
Вопросы:

- Должна ли F-15 включать это или это отдельная feature (F-11.5)?
- Нужен ли отдельный feature.yaml?
