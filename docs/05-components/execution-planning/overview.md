# Компонент: execution-planning

Routing planner для хеджа. Принимает `ExecutionIntent` от Matching Backend (auto-batch) или Gateway (manual override), читает `venue.liquidity.fob` + `venue.health` от F-11, формирует распределение объёма между venues, проходит pre-hedge risk check, передаёт результат в Venue Execution Adapter. Также участвует в fallback routing (UC-F12-04) и retry на reconciliation (UC-F12-03/UC-F12-05).

## Implementation Status

| Целевая ответственность                                            | Реализовано?    | Где                                                                                              |
| ------------------------------------------------------------------ | --------------- | ------------------------------------------------------------------------------------------------ |
| Consume `execution.intents`                                        | ⚠️ stub          | интерфейс [`IExecutionPlanningUseCases`](../../../cpp/matching/src/app/execution_planning_uc.hpp); реализация отсутствует |
| Consume `venue.liquidity.fob` / `venue.health`                     | ❌              | нет consumer'а                                                                                   |
| Routing plan: `qty[v] = L(v) / Sum L * targetQty`                  | ❌              | формула задокументирована (см. [04-domain/business-rules.md](../../04-domain/business-rules.md#routing-plan)), кода нет |
| Filter allowed_venues / venue.health=CONNECTED                     | ⚠️ частично      | [`ExternalVenueFilter`](../../../cpp/matching/src/app/external_venue_filter.cpp) фильтрует venues, но не объединён с health |
| Cache venue inputs (snapshot)                                      | ⚠️ stub          | [`PlannerInputsCache`](../../../cpp/matching/src/app/planner_inputs_cache.cpp)                   |
| Вызов `RiskService.PreHedgeCheck`                                  | ❌              | RPC ещё не определён                                                                             |
| Fallback routing (excluded_venues)                                 | ❌              |                                                                                                  |
| Urgency upgrade при retry                                          | ❌              |                                                                                                  |
| Publish retry intent в `execution.intents`                         | ❌              |                                                                                                  |

## Routing plan algorithm (target)

Базовая формула (IN-005 §6):

$$
\text{qty}[v] = \frac{L(v)}{\sum_{v'} L(v')} \cdot \text{targetQty}, \quad v \in \text{healthy allowed\_venues}
$$

где $L(v)$ — суммарная ликвидность venue $v$ в нужном направлении (`bid` для SELL, `ask` для BUY), извлекаемая из `SideLiquidityCurve` (F-11 `venue.liquidity.fob`).

Дополнительные ограничения:
- $\text{qty}[v]$ округляется к `lot_size[venue][instrument]` через `floor`.
- $\text{qty}[v] \leq \text{maxOrderSize}[venue][instrument]$.
- `venue.health.status != CONNECTED` → venue исключается.
- `venue.health.latency_ms > maxLatencyForUrgency[urgency]` → venue исключается.

Pre-hedge risk check (IN-005 §6, F12-8):

1. $\text{targetNotional} \leq \text{maxNotionalPerHedge}$
2. $\text{currentHedgeExposure}[\text{symbol}] + \text{targetQty} \leq \text{hedgeExposureLimit}[\text{symbol}]$
3. $\text{expectedSlippage} \leq \text{maxSlippage}[\text{urgency}]$

где `expectedSlippage` оценивается из `SideLiquidityCurve` по cumulative cost `s_of_q`.

## Код

- [`cpp/matching/src/app/execution_planning_uc.hpp`](../../../cpp/matching/src/app/execution_planning_uc.hpp) — абстрактный интерфейс.
- [`cpp/matching/src/app/external_venue_filter.cpp`](../../../cpp/matching/src/app/external_venue_filter.cpp) — фильтрация allowed_venues.
- [`cpp/matching/src/app/planner_inputs_cache.cpp`](../../../cpp/matching/src/app/planner_inputs_cache.cpp) — кэш для venue inputs.

## Что должно быть в проде

- Реализация `IExecutionPlanningUseCases` — отдельный сервис или библиотека.
- Подписка на `venue.liquidity.fob` + `venue.health` с обновлением кэша.
- Routing plan с округлением к lot_size и tick_size.
- gRPC client к `RiskService.PreHedgeCheck`.
- Fallback логика на запрос Venue Execution Adapter.
- Идемпотентность по `hedge_flow_id`.
- Метрики: `routing_decisions_total`, `pre_hedge_reject_rate`, `routing_latency_ms`.

## Связанные фичи

- F-12 (Execution Hedge) — основная фича.
- F-11 (External Venues LOB → FOB) — поставщик routing inputs.
- F-04 (Batch Clearing) — через matching эмитирует ExecutionIntent.
- F-07 (Pre-trade Risk) — связь через `PreHedgeCheck`.

## Participates In Features

- [F-12](../../02-system/features/F-12-execution-hedge/)

## Participates In Use Cases

- [UC-F12-01](../../02-system/use-cases/UC-F12-01-auto-hedge-after-batch/use-case.md)
- [UC-F12-02](../../02-system/use-cases/UC-F12-02-manual-operator-hedge/use-case.md)
- [UC-F12-03](../../02-system/use-cases/UC-F12-03-partial-fill-retry/use-case.md)
- [UC-F12-04](../../02-system/use-cases/UC-F12-04-rejection-fallback/use-case.md)
- [UC-F12-05](../../02-system/use-cases/UC-F12-05-timeout-underfilled-reconciliation/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F12-01-auto-hedge-services](../sequences/SEQ-F12-01-auto-hedge-services.md)
- [SEQ-F12-02-rejection-fallback-services](../sequences/SEQ-F12-02-rejection-fallback-services.md)
- [SEQ-F12-03-error-scenarios-services](../sequences/SEQ-F12-03-error-scenarios-services.md)

## Owned Contracts

- internal `RoutingPlan` value object (не в proto).

## Produced Events

- [execution.intents](../../06-api/messaging/execution-intents.md) (retry intents)

## Consumed Events

- [execution.intents](../../06-api/messaging/execution-intents.md)
- [venue.liquidity.fob](../../06-api/messaging/venue-liquidity-fob.md) (F-11)
- [venue.health](../../06-api/messaging/venue-health.md) (F-11)

## Data Access

- stateless; кэш venue inputs в памяти.
