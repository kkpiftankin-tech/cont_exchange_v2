# F-20 Live Venue Simulator — implementation tasks

Источник: IN-010. Feature: [F-20](../02-system/features/F-20-live-venue-simulator/README.md).
`IMPLEMENTATION_HINT`-артефакт (docs-as-code). Код (`cpp/`, `infra/`, `contracts/proto/`)
**не** трогать, пока пользователь явно не попросит реализацию.

> **Базовый симулятор уже есть** — `cpp/venues/src/app/venues_loop.cpp` (Поток-2:
> `execution.intents` → «filled» `ExecutionReport` в `execution.reports`). F-20 — это
> его **эволюция**: live-LOB, модели, маршрутизация, sim-сессии, SHADOW.

## Preconditions (docs-gate)

- [x] feature.yaml + README (F-20)
- [x] UC-F20-01 (SIM_ONLY) + UC-F20-02 (SHADOW) + L0/L1 sequences
- [x] FR-F20-001..011 + NFR-F20-001..007
- [x] domain entities (VenueSimulator/Router/SimSession/модели) + business-rules §F-20
- [x] contracts docs (messaging/sim-topics, rest/sim-sessions-admin-api) + data docs (sim-sessions, sim-execution-reports)
- [x] test-plan
- [ ] **ADR: топики + proto-расширение** (CN-F20-02/04) — решение владельца до кода
- [ ] **ADR/decision: sim-book в Ledger** (CN-F20-03)

## Phase 0. ADR / решения владельца

- T-F20-001: ADR — имена топиков (`execution.venue` источника vs `execution.reports`/`execution.intents` в репо) + новые `sim.config`/`sim.alerts`/`sim.execution.venue`.
- T-F20-002: ADR — расширение `execution.proto` (ExecutionReport + sim-поля, backward-compat) или отдельный `sim_execution.proto`; деньги = Decimal.
- T-F20-003: ADR — sim-книга в Ledger (изоляция боевых позиций, R-F20-001).

## Phase 1. Контракты и инфраструктура (после ADR)

- T-F20-101: proto-изменение по T-F20-002 + proto-map.
- T-F20-102: `infra/kafka/create_topics.sh` — `sim.config`, `sim.alerts`, `sim.execution.venue`.
- T-F20-103: `infra/postgres/init.sql` — `sim_sessions`, `sim_child_orders`.
- T-F20-104: `infra/clickhouse/init.sql` — `sim_execution_reports`, `sim_divergence_log`.

## Phase 2. VenueSimRouter

- T-F20-201: карта `venueId+symbol → routingMode`; consume `sim.config` (hot reload).
- T-F20-202: SIM_ONLY / LIVE_ONLY проксинг; SHADOW fork (par LIVE+SIM).
- T-F20-203: атомарное переключение ≤ 500мс (AC-F20-07), LIVE overhead ≤ 5мс (AC-F20-12).

## Phase 3. VenueSimulator (ядро)

- T-F20-301: подписка `venue.snapshots`, LOB-кэш `[venueId][symbol]`, `lobAge`/stale-guard (R-F20-002).
- T-F20-302: LEVEL_BY_LEVEL matching + VWAP (алгоритм TRD 4.4).
- T-F20-303: ImpactModel (LINEAR/SQRT/POWER_LAW/LEVEL_BY_LEVEL) + `impactBps`.
- T-F20-304: FeeModel + RejectionModel (taxonomy R-F20-006).
- T-F20-305: LatencyModel + async delay; SIM_TIMEOUT.
- T-F20-306: сборка `SimExecutionReport` (sim-поля) + publish в `execution.*` (+ dup).

## Phase 4. SimSession Manager + Admin API

- T-F20-401: CRUD `/sim/sessions` (REST) → `sim_sessions` + publish `sim.config`.
- T-F20-402: жизненный цикл ACTIVE/PAUSED/COMPLETED/CANCELLED.

## Phase 5. Ledger sim-book + Divergence

- T-F20-501: Ledger ветвление `simMode=true` → изолированная sim-книга (R-F20-001, AC-F20-05).
- T-F20-502: Divergence Service (SHADOW): пары по `clientOrderId` → `sim_divergence_log` (AC-F20-10).

## Phase 6. Observability + Admin UI

- T-F20-601: дашборды Sim Monitor / Sim vs Live / LOB Quality / Impact Analysis; алерты (`sim.alerts`).
- T-F20-602: Admin UI — SimSession Manager, Live Sim Feed, Sim vs Live, Model Calibration Panel.

## Phase 7. Тесты

- T-F20-701..: unit U1–U10, integration IT-1..5, ledger L1, load LT-1..4, SLA (см. [test-plan](../10-testing/features/F-20-test-plan.md)).

## Критический путь

ADR (Phase 0) → контракты/infra (Phase 1) → Simulator ядро (Phase 3) → Router (Phase 2) →
Ledger sim-book (Phase 5). Переиспользует F-11 (`venue.snapshots`) и F-12 (child-order/report).
