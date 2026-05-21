---
id: ADR-013
status: draft
date: 2026-05-20
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-12-execution-hedge/
  - docs/05-components/execution-planning/overview.md
  - docs/05-components/matching-fob-core/overview.md
  - docs/05-components/venue-execution-adapter/overview.md
  - incoming-docs/2026-05-20-F-12-execution-hedge-v1.md
---

# ADR-013: Execution Planning — отдельный сервис, библиотека внутри matching, или модуль внутри venue-execution-adapter?

## Контекст

IN-005 описывает **Execution Planning** как самостоятельный компонент, ответственный за:

- consume `execution.intents`;
- agregate `venue.liquidity.fob` + `venue.health` (F-11);
- routing plan: `qty[v] = L(v) / Sum L * targetQty`;
- pre-hedge risk check (gRPC `RiskService.PreHedgeCheck`);
- fallback routing при rejection;
- retry с urgency upgrade при reconciliation gap;
- передача `ExecutionIntent + RoutingPlan` в Venue Execution Adapter.

В текущем коде есть только абстрактный интерфейс [`IExecutionPlanningUseCases`](../../cpp/matching/src/app/execution_planning_uc.hpp) в `cpp/matching/src/app/`. Реализации нет.

Вопрос: где разместить реализацию?

## Решение

**Реализовать Execution Planning как отдельный микросервис `cpp/execution_planning/`** в production-итерации F-12. На первом MVP-шаге допустимо собрать его как library внутри `cpp/venue_execution_adapter/` (или `cpp/venues/`), чтобы избежать лишнего сетевого hop'а.

Условие для split в отдельный сервис:

1. Routing logic усложняется (ILP / cost-minimization вместо proportional);
2. Несколько consumers ожидают routing decisions (например, telemetry, simulation);
3. Хочется деплоить planning с отличной шкалой (например, более CPU-heavy).

## Почему **не** оставлять Planning внутри matching

CLAUDE.md §10.3 явно говорит: `matching не решает matching хеджа` (`matching не торгует`, `venues не принимает бизнес-решения о хеджировании`). Execution Planning принимает бизнес-решения о routing — это **не** matching domain. Логичнее держать рядом с `venue-execution-adapter`, потому что:

- Planning потребляет `venue.liquidity.fob` (от F-11) — тот же source, что и Adapter использует через EVC.
- Planning вызывает `RiskService.PreHedgeCheck` — тот же путь, что и Adapter при validation.
- Planning отдаёт результат напрямую в Adapter (1:1 ratio decisions/intents).

## Альтернативы

### 1. Внутри matching (как extension F-04 batch loop)

**Pros:** matching уже эмитит ExecutionIntent, можно прямо после batch tier сделать routing.
**Cons:** нарушает SRP (matching и так делает clearing + emit; добавление routing раздувает loop); затрудняет manual override (UC-F12-02 не через matching).

Отклонено.

### 2. Внутри venue-execution-adapter (как часть `cpp/venues/`)

**Pros:** один процесс, нулевой network overhead, общий PostgreSQL connection.
**Cons:** Planning и Adapter имеют разные потребности (Planning consume `venue.liquidity.fob`, Adapter consume `execution.intents`); смешение state.

Acceptable как **первый MVP-шаг**, потом split.

### 3. (Выбрано) Отдельный сервис `cpp/execution_planning/` для production; MVP-shortcut как library внутри `cpp/venues/`

**Pros:** соответствует логической декомпозиции IN-005; чёткие границы; легко масштабировать independently; легко заменить алгоритм routing.
**Cons:** ещё один Kafka consumer; +1 deployment unit; +1 health check.

### 4. Routing-algorithm как стандартный shared library без сервиса

**Pros:** простая декомпозиция, любой сервис может использовать.
**Cons:** state cache (venue.liquidity.fob snapshots) нужно держать synced — что в shared library трудно.

Отклонено.

## Phased Implementation

| Phase | Решение |
| --- | --- |
| F-12 MVP (T-F12-401..404) | Execution Planning как module внутри `cpp/venues/src/app/execution_planning/` (тот же бинарник, что Adapter) |
| F-12 v1.1 production | Вынести в отдельный сервис `cpp/execution_planning/` с собственным Dockerfile, Kafka consumer group, gRPC client к RiskService |
| F-12 v2 (advanced routing) | Replaceable algorithm (proportional → ILP / RL-based) через DI |

## Routing Algorithm

См. формулу в [04-domain/business-rules.md → Routing Plan](../../04-domain/business-rules.md#routing-plan).

Кандидаты для будущей замены (open question §3 в F-12):

- **Proportional** (текущая базовая): простая, deterministic.
- **ILP minimization:** минимизация expected_slippage + fees + venue_health_penalty при constraints lot_size, maxOrderSize, healthy_venues.
- **RL-based:** policy learnt from agent.logs (F-12 как state-action log).

Выбор фиксируется в отдельной ADR (TBD).

## Последствия

### Положительные

- Соответствие logical decomposition IN-005.
- Возможность независимо тестировать routing logic.
- Чёткая граница для будущего advanced routing.

### Отрицательные

- В MVP — added module complexity внутри `cpp/venues/`.
- В production — extra Kafka consumer + service deployment.

### Обратимость

Высокая для phase 1 (module → service refactor — стандартная процедура). Низкая для замены routing algorithm после деплоя (требует replay parity).

## Open Questions

См. [F-12 open-questions §2, §3](../../02-system/features/F-12-execution-hedge/open-questions.md).

## Status

Draft. Ожидает review.
