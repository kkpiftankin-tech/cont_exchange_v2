<!--
---
id: UC-F05A-06
title: "Run CE Tact with Virtual Counterparties (agents, nodes, 3-level position)"
level: sea
parent-feature: F-05A
system-sequence: "sequences/SEQ-UC-F05A-06-system.md"
service-sequence: "../../../05-components/sequences/SEQ-F05A-UC-F05A-06-services.md"
---
-->

# UC-F05A-06. Run CE Tact with Virtual Counterparties

## Feature

- [F-05A. Vectorized External Liquidity](../../features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Primary Actor

Matching Backend (node vector solver)

## Supporting Actors

- Market Data Service (`agent_builder`: снапшоты → агенты → сегменты)
- Ledger (узловые остатки `asset@venue`, `GetNodeBalances`)
- Risk Manager (агрегат `Z_a`, форматтер заявок)
- External Venues Connector (исполнение поручений)
- ClickHouse (`clearing_trace`, `node_balances_history`), Kafka

## Preconditions

- Снапшоты книг всех площадок собраны на общий момент окна такта ([ADR-056](../../../03-architecture/adr/ADR-056-ce-node-graph-clearing.md); окно [ADR-050](../../../03-architecture/adr/ADR-050-f05a-window-aggregation.md)).
- Построен базис узлов `asset@venue` + `asset@__book__` ([ADR-056](../../../03-architecture/adr/ADR-056-ce-node-graph-clearing.md)).
- Агенты (переводчик/связка) с `(α, c, σ*)` построены из снапшотов ([ADR-055](../../../03-architecture/adr/ADR-055-ce-virtual-counterparties-agents.md)).
- Конфиг такта (`theta, dhl_fraction, rho, z_limit, gamma`) в `f05a_clearing_config`.

## Trigger

Оконный агрегатор `market-data` отдаёт срез книг с общим `batch_id`.

## Main Flow

1. **A0.** Сбор снапшотов на общий момент; площадка со снапшотом старше `stale_level_ms`
   исключается и помечается `EXCLUDED_STALE`.
2. **A1–A2.** `market-data` строит агентов: якорь `σ*` (mid, сдвинут на давление
   собственного остатка и `committed`), глубина `α`, мёртвая зона `c`; каждый агент →
   два односторонних сегмента с мёртвой зоной ([ADR-055](../../../03-architecture/adr/ADR-055-ce-virtual-counterparties-agents.md)).
3. **A2 (агрегатный овеппрайд).** При `|Z_a| > z_limit` к скоростям плеч актива `a`
   добавляется `−ρ_lim·(Z_a − sign·z_limit)` — единственный вход агрегата в клиринг ([ADR-057](../../../03-architecture/adr/ADR-057-ce-three-level-position.md)).
4. **A3.** Сборка графа: узлы площадок балансируются (`Wx=0`), узлы книги свободны и несут
   марку `μ_a` в линейном члене ([ADR-056](../../../03-architecture/adr/ADR-056-ce-node-graph-clearing.md)).
5. **A4.** Клиринг QP `min_x Σ[½·m_i·x_i² + a_i·x_i]` s.t. `Wx=0` по узлам площадок,
   box запаса по узлам книги. Выход: `x`, цены узлов `pi`, `residual`; поток агента
   `f = x_sell − x_buy`.
6. **A5.** Проверка инвариантов И-Т1…И-Т6 как стадия конвейера (до эмиссии) ([ADR-059](../../../03-architecture/adr/ADR-059-ce-clearing-trace-audit.md)).
7. **A6.** Пересчёт №1: `committed_e += |f_e|` (транзакция с эмиссией); факт узлов не трогается ([ADR-058](../../../03-architecture/adr/ADR-058-ce-committed-inflight-state.md)).
8. **A7.** Эмиссия поручений из потоков: QUOTE → ордер в книгу площадки, TRANSFER →
   вывод/депозит, STOCK → проводка. Тип заявки (пассив/рынок/IOC) из экспозиции `Z_a` ([ADR-057](../../../03-architecture/adr/ADR-057-ce-three-level-position.md)).
9. **A8.** Пересчёт №2: подтверждения `execution.reports` меняют факт узлов `Q_{a,j}`,
   снимают `committed`, пересчитывают `Z_a`.
10. **A9.** Публикация `ClearingTrace` (агенты/узлы/инварианты/PnL/позиции/заявки) →
    `matching.clearing_trace` + ClickHouse; `ce.position.delta` → ledger.

## Alternative Flows

### A1. Нарушение инварианта баланса/позиции (И-Т1 / И-Т3)

1. `max_u |(Wx)_u| ≥ ε` или расхождение позиции тремя способами → такт `FAILED`.
2. Заявки не эмитятся; событие в `risk.alerts`; трасса помечается `failed`.

### A2. Разрыв цен меньше суммы мёртвых зон маршрута

1. `ε ≤ Σ_{e∈маршрут} c_e` → все потоки маршрута нули (агенты молчат) — не ошибка, а
   ожидаемый исход ([ADR-055](../../../03-architecture/adr/ADR-055-ce-virtual-counterparties-agents.md) §8).

### A3. Нога неисполнима (И-Т4)

1. `Q_{a,j} + Δ_немедленное < 0` по узлу → нога режется или закрывается срочно по рынку.

## Postconditions

- Опубликована `ClearingTrace` с общим `batch_id`; инварианты зафиксированы (passed/failed).
- Позиция обновлена на трёх уровнях (агент/узел/актив); `committed` учтён ([ADR-057](../../../03-architecture/adr/ADR-057-ce-three-level-position.md), [ADR-058](../../../03-architecture/adr/ADR-058-ce-committed-inflight-state.md)).
- Ledger сбалансирован по узлам площадок; позиция биржи наблюдаема (не ≡ 0).

## Related Sequence Diagrams

- System sequence: [sequences/SEQ-UC-F05A-06-system.md](sequences/SEQ-UC-F05A-06-system.md)
- Service sequence: [../../../05-components/sequences/SEQ-F05A-UC-F05A-06-services.md](../../../05-components/sequences/SEQ-F05A-UC-F05A-06-services.md)

## Related Contracts

- `contracts/proto/fob/agent/v1/agent.proto` (planned), `contracts/proto/fob/marketdata/v1/vector_liquidity.proto` (`NodeBasis`, planned)
- `contracts/proto/fob/ledger/v1/ledger.proto` (`GetNodeBalances`, planned), `contracts/proto/fob/risk/v1/risk.proto` (`Z_a`, planned)
- Kafka `marketdata.vectorized`, `ce.position.delta`, `matching.clearing_trace` (planned), `execution.intents`, `execution.reports`

## Related Components

- `market-data`, `matching`, `ledger`, `risk`, `venues`

## Related Data

- PG `agent_state`, `node_balances`, `f05a_clearing_config` (planned columns)
- CH `clearing_trace`, `node_balances_history`, `agent_state_history` (planned), `vector_flow_segments_history` (+9 cols)

## Related ADR

- [ADR-055 виртуальные контрагенты как агенты](../../../03-architecture/adr/ADR-055-ce-virtual-counterparties-agents.md)
- [ADR-056 граф узлов, снятие Wx=0](../../../03-architecture/adr/ADR-056-ce-node-graph-clearing.md)
- [ADR-057 трёхуровневая позиция](../../../03-architecture/adr/ADR-057-ce-three-level-position.md)
- [ADR-058 committed/in-flight](../../../03-architecture/adr/ADR-058-ce-committed-inflight-state.md)
- [ADR-059 clearing_trace](../../../03-architecture/adr/ADR-059-ce-clearing-trace-audit.md)
