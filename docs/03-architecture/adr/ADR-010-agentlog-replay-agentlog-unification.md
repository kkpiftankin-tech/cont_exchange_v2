# ADR-010 — AgentLog vs ReplayAgentLog: unification strategy

- **Status:** Proposed
- **Date:** 2026-05-20
- **Deciders:** core-team

## Контекст

В проекте есть два контекста, генерирующих `state-action-reward` журналы:

1. **Production matching** — потенциально RL-агенты в будущем смогут
   принимать live trading решения. Журналы такого агента упомянуты в
   IN-001 как `agent_logs` и в `contracts/proto/fob/agent/v1/agent.proto`
   как `AgentLog`. Соответствующая ClickHouse таблица — `agent_logs`
   (planned).
2. **F-15 Backtest / Replay** — каждый replay-батч пишет в
   `replay_agentlogs` (ClickHouse) с расширенным набором полей: state,
   action, reward + diagnostics (`solver_error_flag`, `risk_status`,
   `failure_component`, `batch_result_json`, `fills_json`, ...).

Эти две концепции пересекаются по семантике, но отличаются по контексту
(live vs replay) и набору полей (replay имеет привязку к `session_id` и
полную diagnostics; live имеет только сам state-action-reward + ссылку
на `batch_id`).

## Альтернативы

- **(A) Single unified table.** Одна таблица `agent_logs` для обоих
  контекстов, с дискриминатором `source = live|replay` и опциональными
  replay-specific полями.
- **(B) Two separate tables (текущее состояние).** `agent_logs` для live
  и `replay_agentlogs` для F-15. Сходство — на уровне schema
  documentation, не физического слоя.
- **(C) View / materialized aggregation.** Две таблицы + общий view
  `unified_agent_logs` для analytics.

## Решение (proposed)

Принять **вариант B** на ближайший горизонт (MVP F-15 → production
hardening). Когда production-RL-агенты будут актуальны, рассмотреть
вариант C для unified analytics, не объединяя физический слой.

## Обоснование

- **Replay-specific diagnostics** (`solver_error_flag`, `failure_component`,
  `session_config_snapshot_version`, `risk_status`, `error_code`) не имеют
  смысла в live контексте. Включение их в unified table ведёт к
  раздутию пустых колонок и проблемам индексов.
- **Retention отличается.** F-15 replay logs: 90 дней (configurable).
  Live agent logs: возможно регуляторное требование 5+ лет. Разные TTL.
- **Ownership разный.** F-15 пишется backtest-service; live agent logs
  будут писаться matching-service или отдельным policy-engine.
- **Сложности с partitioning.** Замешивание сильно различающихся объёмов
  ухудшит query performance.
- **Унификация на уровне READ** через view (вариант C) сохраняет всю
  гибкость без compromise.

## Trade-offs / последствия

- **Code duplication.** Возможна повторная реализация AgentLog
  builders в matching service при появлении live policy. Mitigation:
  вынести общий core в `cpp/common` (например, `agent_log_core.hpp`).
- **Schema drift risk.** State / Action JSON форматы могут разойтись
  между live и replay. Mitigation: общая proto definition
  `fob.agent.v1.AgentState` + `AgentAction`.
- **No instant analytics.** Объединённые отчёты потребуют JOIN/UNION
  поверх двух таблиц. Mitigation: materialized view, когда будет нужно.

## Обратимость

Reversible. Если позже потребуется консолидация:

1. Создать унифицированную таблицу `agent_logs_v2` со всеми полями.
2. Перенести данные через bulk INSERT.
3. Переключить producers по очереди.

Без изменений в use cases.

## Связанные артефакты

- IN-001 § agent_logs (legacy reference)
- `contracts/proto/fob/agent/v1/agent.proto` (live AgentLog proto)
- [docs/07-data/replay-agentlogs.md](../../07-data/replay-agentlogs.md) (F-15 replay table)
- [docs/02-system/features/F-15-backtest-replay/open-questions.md §4](../../02-system/features/F-15-backtest-replay/open-questions.md)

## Source

- IN-001-FR-027, IN-001-FR-028 (vision agent logs)
- IN-006 § AgentLog (state-action-reward)
