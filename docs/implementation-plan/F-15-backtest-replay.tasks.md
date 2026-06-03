# Implementation Tasks: F-15 Backtest / Replay

## Progress

F-15 — самая зрелая фича по покрытию кода в репозитории. Полный сервис
`cpp/backtest` импортирован из `origin/dev` вместе с миграциями, тестами и
docker-compose интеграцией. Документация в `docs/` догоняет код, не наоборот.

| Артефакт | Что сделано | Статус |
| --- | --- | --- |
| Code: cpp/backtest | 19 use cases + 13 ports + 12 infra adapters | ✅ |
| Migrations | 001 (replay schema), 002 (RBAC), 003 (retry_parent_id) | ✅ |
| Proto contracts | contracts/proto/fob/replay/v1/replay.proto | ✅ |
| OpenAPI | contracts/openapi/fob/replay/v1/api/replay.yaml | ✅ |
| Tests | 27 unit + 1 integration + 1 load harness + Testing/f15_*.{sh,py} | ✅ |
| Docker compose | infra/docker-compose.dev.yml service `backtest` port 8087 | ✅ |
| Kafka topics | replay.commands, replay.results в create_topics.sh | ✅ |
| Docs: feature/UC/sequences/data/api (этот PR) | Полный набор после IN-006 ingest | ✅ |

## Source Artifacts

- Feature: [F-15 Backtest / Replay](../02-system/features/F-15-backtest-replay/)
- Use cases:
  - [UC-F15-01 Create](../02-system/use-cases/UC-F15-01-create-replay-session/use-case.md)
  - [UC-F15-02 Cancel](../02-system/use-cases/UC-F15-02-cancel-replay-session/use-case.md)
  - [UC-F15-03 Compare](../02-system/use-cases/UC-F15-03-ab-compare-sessions/use-case.md)
  - [UC-F15-04 Audit](../02-system/use-cases/UC-F15-04-audit-mode-replay/use-case.md)
  - [UC-F15-05 Retry](../02-system/use-cases/UC-F15-05-retry-failed-session/use-case.md)
  - [UC-F15-06 Determinism](../02-system/use-cases/UC-F15-06-replay-determinism-check/use-case.md)
- Service sequences: SEQ-F15-01..SEQ-F15-04 в [05-components/sequences/](../05-components/sequences/)
- Component: [backtest-service](../05-components/backtest-service/overview.md)
- Contracts: [REST](../06-api/rest/replay.md), [Kafka](../06-api/messaging/replay-topics.md), [Proto](../../contracts/proto/fob/replay/v1/replay.proto), [OpenAPI](../../contracts/openapi/fob/replay/v1/api/replay.yaml)
- Data: [replay-sessions](../07-data/replay-sessions.md), [replay-summaries](../07-data/replay-summaries.md), [replay-agentlogs](../07-data/replay-agentlogs.md), [replay-rbac](../07-data/replay-rbac.md)
- Test plan: [F-15-test-plan.md](../10-testing/features/F-15-test-plan.md)
- IN-006 fragments: см. [`incoming-docs/IN-006.fragment-map.md`](../../incoming-docs/IN-006.fragment-map.md)

## Preconditions

- [x] Feature exists
- [x] Use cases exist (6)
- [x] System-level sequences exist (6)
- [x] Service-level sequences exist (4)
- [x] Contracts exist (REST + Kafka + proto)
- [x] Data objects exist (4 PG tables + 1 CH table + RBAC tables)
- [x] Acceptance criteria exist
- [x] Code shipped (19 use cases)

## Tasks (только gap'ы)

### T-F15-001. WebSocket bridge для replay.results

**Status:** ❌ Not started.

Target:

- В gateway добавить WebSocket endpoint `/ws/v1/replay/{session_id}/progress`,
  который подписывается на Kafka `replay.results` (filter по `session_id`)
  и пушит сообщения клиенту.

Acceptance:

- UI получает live progress без polling.
- При `ReplayCompletedEvent` / `ReplayFailedEvent` соединение закрывается.

References: F15-15 (DoD), `KafkaReplayEventPublisher`.

### T-F15-002. Web UI экраны

**Status:** ❌ Not started.

Target:

1. **Backtest / Replay form** — POST /sessions, валидация strategy.
2. **Session List** — таблица с фильтрами.
3. **Session Details** — KPI cards, equity curve, AgentLog table с пагинацией.
4. **A/B Compare** — side-by-side delta matrix.
5. **Audit-mode** — single batch + diff визуализация.

Acceptance:

- Все экраны переключают табы без потери данных.
- WebSocket прогресс отображается в real-time.

References: DoD §16, IN-006 § UI экраны.

### T-F15-003. User guide / tech description

**Status:** ⚠️ partial (только OpenAPI + docs-as-code).

Target:

- `docs/11-operations/replay-user-guide.md` с пошаговыми сценариями для аналитика.
- `docs/11-operations/replay-runbook.md` для оператора (restart failed session, очистка cache, retention overrides).

References: DoD §22.

### T-F15-004. Сохранение determinism_check_results

**Status:** ⚠️ partial (UC реализован, но результат не персистируется).

Target:

- Добавить таблицу `determinism_check_results` (PG) или хранить в JSONB.
- API: GET /api/v1/replay/determinism-checks/{id}.

References: UC-F15-06.

### T-F15-005. Перенести DDL replay_agentlogs в init.sql (опционально)

**Status:** ⚠️ design decision pending.

Target (если решение положительное по ADR):

- Перенести CREATE TABLE replay_agentlogs из clickhouse_storage.cpp в
  infra/clickhouse/init.sql.
- Оставить ALTER MODIFY TTL на стороне сервиса для конфигурируемого retention.

Decision: см. [ADR-011-replay-agentlogs-ddl-placement.md](../03-architecture/adr/ADR-011-replay-agentlogs-ddl-placement.md) (если будет создан).

References: Conflict Note CN-IN006-04.

### T-F15-006. Stress 20-сессий → solve time p95

**Status:** ⚠️ harness готов, SLO иногда не проходит.

Target:

- Профилировать `f15_load_harness --mode=stress20`.
- Оптимизировать ClickHouse INSERT batching или ввести throttling.

References: AC-N5 в [acceptance-criteria.md](../02-system/features/F-15-backtest-replay/acceptance-criteria.md#2-нефункциональные-требования).

### T-F15-007. Лимит/таймаут на SSE-стримы replay в gateway

**Status:** ❌ открыт (обнаружено 2026-06-03 при E2E persist-фичи).

Проблема: эндпоинт `GET /api/v1/replay/results/stream` (SSE) в `cpp/gateway/src/transport/http_gateway.cpp`
держит соединение открытым без idle/max-duration таймаута и без лимита на число
одновременных стримов. Долгоживущие SSE-соединения (проксируются `frontend-api` для
каждой открытой вкладки `/replay`, плюс реконнекты) накапливаются и исчерпывают пул
обработчиков Crow → gateway перестаёт принимать новые соединения (healthz и create
начинают таймаутить, `502 "operation was aborted"` со стороны frontend-api). Воспроизведено:
после серии прогонов `healthz` отдавал `HTTP=000`, лечилось `docker restart infra-gateway-1`.

Target:

- Ввести idle-timeout и max-duration на SSE-хэндлер (`/results/stream`), закрывать
  завершённые/протухшие стримы.
- Ограничить число одновременных SSE-подписок (per-session и глобально); лишние —
  `429`/`503` с Retry-After.
- Рассмотреть неблокирующую отдачу стрима, чтобы один стрим не занимал воркер-тред Crow.
- Добавить метрики: число активных SSE-стримов, отклонённых по лимиту, средняя длительность.
- Тест: открыть N стримов > пула, убедиться что `healthz`/create продолжают отвечать.

References: обнаружено при валидации [docs/06-api/rest/replay.md](../06-api/rest/replay.md) (`persist`),
наблюдаемость — F-17.

## Definition of Done (IN-006)

См. [acceptance-criteria.md §4](../02-system/features/F-15-backtest-replay/acceptance-criteria.md#4-definition-of-done--соответствие-in-006-22-пункта). Покрытие 19/22 ✅, 2/22 ⚠ (WebSocket, user-guide), 1/22 ❌ (Web UI).

## Source Fragments

- IN-006 § Definition of Done (22 пункта)
