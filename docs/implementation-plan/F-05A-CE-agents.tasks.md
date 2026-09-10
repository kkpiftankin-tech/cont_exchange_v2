# Implementation Tasks: F-05A CE Virtual Counterparties (agents, node graph, 3-level position)

## Source Artifacts

- Source: **IN-0XX** — `CE_algorithm_spec.md`, `CE_virtual_counterparties.md` (⧗ регистрация ingest-docs
  ожидает пере-отправки файлов: хук авто-архивации не сохранил вложения в `incoming-docs/`).
- Feature: [F-05A Vectorized External Liquidity](../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)
- Use case: [UC-F05A-06 Run CE Tact with Virtual Counterparties](../02-system/use-cases/UC-F05A-06-ce-agent-tact-clearing/use-case.md)
  ([L0](../02-system/use-cases/UC-F05A-06-ce-agent-tact-clearing/sequences/SEQ-UC-F05A-06-system.md),
  [L1](../05-components/sequences/SEQ-F05A-UC-F05A-06-services.md))
- ADRs (accepted 2026-09-10): [ADR-055](../03-architecture/adr/ADR-055-ce-virtual-counterparties-agents.md),
  [ADR-056](../03-architecture/adr/ADR-056-ce-node-graph-clearing.md),
  [ADR-057](../03-architecture/adr/ADR-057-ce-three-level-position.md),
  [ADR-058](../03-architecture/adr/ADR-058-ce-committed-inflight-state.md),
  [ADR-059](../03-architecture/adr/ADR-059-ce-clearing-trace-audit.md)
- Расширяет: [ADR-052](../03-architecture/adr/ADR-052-f05a-two-sided-signed-segment.md) (two-sided),
  [ADR-054](../03-architecture/adr/ADR-054-ce-capital-net-position-hedging.md) (NOP → частный случай `Z_a`)

## Preconditions (docs-as-code gate)

- [x] ADR-055…059 → `accepted` (2026-09-10)
- [x] UC-F05A-06 + L0 system sequence + L1 service sequence (contract + data binding)
- [x] Feature.yaml: UC-F05A-06 + ADR-055…059 в `architectureDecisions`
- [ ] Регистрация IN-0XX (ingest-docs) — ⧗ ждёт пере-отправки исходников
- [x] Contracts: `agent.proto`, `vector_liquidity.proto` (`NodeBasis`), `ledger.proto` (`GetNodeBalances`/`ApplyNodeTransfer`), `risk.proto` (`Z_a`) + `docs/06-api/*` — protoc 29.3 OK (commit a7216691)
- [x] Data schemas (docs): [ce-agent-nodes.md](../07-data/ce-agent-nodes.md) — PG `agent_state`/`node_balances`/`f05a_clearing_config`(+5); CH `clearing_trace`/`node_balances_history`/`agent_state_history`/`vector_flow_segments_history`(+9). DDL — при реализации.
- [x] Test plan (docs): [F-05A-CE-agents-test-plan.md](../10-testing/features/F-05A-CE-agents-test-plan.md) — U/G/V/E + инварианты И-Т1…И-Т6
- [ ] Python-эталон `agents_vc.py`/`agents_sim.py` — ⧗ не приложен (нужен для G-CEA golden)

## Conflict Notes (источник vs фактический код 1e8bf6f)

| # | Утверждение источника | Факт | Действие |
| --- | --- | --- | --- |
| CN-1 | matching не собирается: `solve.pi` нет, конструктор 4-арг, «5 ошибок» | `pi` есть (`vector_qp_solver.hpp:117`), конструктор 5-арг (139-143), собран, `ce.position.delta` e2e | не «чинить»; вычеркнуть из Этапа 0 |
| CN-2 | секция позиция биржи пуста (proto drift) | `/api/vector-clearing/detail` возвращает `cePosition` | proto drift устранён; закрепить автогенерацией копии |
| CN-3 | таблицы `f05a_clearing_config` нет | таблица есть (`postgres/init.sql:942`), нужны лишь +5 колонок | ALTER, не CREATE |

## Critical path

`Этап 0 (реальный остаток + автоген proto)` → `Этап 1 (узлы + агенты + снятие Wx=0)` →
`Этап 2 (позиция 3 уровней + committed)` → `Этап 3 (исполнение)` → `Этап 4 (clearing_trace + фронт)`.
Главный полюс — Этап 1 (граф узлов + двусторонние агенты).

---

## Этап 0 — реальный остаток (не блокирует ничего, ~1–2 дня)

| ID | Задача | Файлы | П | Acceptance |
| --- | --- | --- | --- | --- |
| T-CEA-001 | +9 колонок в `vector_flow_segments_history` + синхронный писатель | [clickhouse/init.sql](../../infra/clickhouse/init.sql), [clickhouse_vector_segment_storage.cpp](../../cpp/market_data/src/infra/clickhouse/clickhouse_vector_segment_storage.cpp) | П-15 | `/api/vector-clearing/detail` секция 1 отдаёт сегменты без 502 |
| T-CEA-002 | +5 колонок в `f05a_clearing_config` (`theta, dhl_fraction, rho, z_limit, gamma`) ALTER | [postgres/init.sql:942](../../infra/postgres/init.sql) | П-12 | форма конфига Clearing читается без ошибки |
| T-CEA-003 | Автогенерация proto-копии BFF (убрать ручной `COPY proto`) | [frontend/api/Dockerfile](../../frontend/api/Dockerfile) | §4 | копия proto собирается из `contracts/`, дрейф невозможен |
| T-CEA-004 | Починка хука авто-архивации (немое падение на `<document>`) | [tools/auto-archive-attachments.py](../../tools/auto-archive-attachments.py) | — | self-test + реальное вложение архивируется в `incoming-docs/` |

## Этап 1 — узлы и двусторонние агенты (~2–3 нед) — ADR-055/056

| ID | Задача | Файлы | П | Acceptance |
| --- | --- | --- | --- | --- |
| T-CEA-101 | `agent.proto`: `Agent/AgentType/AgentLeg/AgentState/AgentAssignment` | [agent.proto](../../contracts/proto/fob/agent/v1/agent.proto) | §4 | компилируется; docs/06-api обновлён |
| T-CEA-102 | `NodeBasis` / формат ключа `ASSET@VENUE`; сегмент += `agent_id, anchor, dead_zone, alpha` | [vector_liquidity.proto](../../contracts/proto/fob/marketdata/v1/vector_liquidity.proto) | §4 | аддитивно, старые consumers целы |
| T-CEA-103 | Оконный агрегатор снапшотов (общий `batch_id` окна, не `snapshot_id`) | [market_data_uc.cpp:246-262](../../cpp/market_data/src/app/market_data_uc.cpp) | П-1 | один такт = окно по всем площадкам |
| T-CEA-104 | `agent_builder.{hpp,cpp}`: mid/α/c/anchor из стакана (детерминизм для replay) | `cpp/market_data/src/domain/agent_builder.*` (new) | П-2 | unit: снапшот → агенты, повтор даёт тот же результат |
| T-CEA-105 | `BuildNodeBasis` вместо `BuildAssetBasis` | [vectorize.hpp:46](../../cpp/market_data/src/domain/vectorize.hpp) | П-4 | базис узлов, стабильный порядок |
| T-CEA-106 | `AgentToSegments`: 2 односторонних сегмента с мёртвой зоной `σ*∓c` | [vectorize.cpp:92-98](../../cpp/market_data/src/domain/vectorize.cpp) | П-6 | закрытая форма PnL сходится |
| T-CEA-107 | Снятие `Wx=0`: box запаса по узлам книги (`l=−q, u=+q`), марка μ в лин.члене | [vector_qp_solver.cpp:56-58](../../cpp/matching/src/domain/vector_qp_solver.cpp) | Ш-2, П-5 | позиция биржи ≠ 0 при разных mid |

## Этап 2 — позиция трёх уровней + committed (~2 нед) — ADR-057/058

| ID | Задача | Файлы | П | Acceptance |
| --- | --- | --- | --- | --- |
| T-CEA-201 | `ledger.GetNodeBalances`/`ApplyNodeTransfer`; остатки `(account,asset,venue)`; двойная запись | [ledger_uc.cpp:612](../../cpp/ledger/src/app/ledger_uc.cpp), [ledger.proto](../../contracts/proto/fob/ledger/v1/ledger.proto) | П-8 | остатки по узлам; `ApplyPositionDelta` со встречной ногой |
| T-CEA-202 | PG `node_balances`, `agent_state` | [postgres/init.sql](../../infra/postgres/init.sql) | П-8, П-10 | миграции + DAO |
| T-CEA-203 | `risk`: `Z_a = Σqty + in_transit + committed − target` вместо `assets−client` | [risk_uc.cpp:~1112](../../cpp/risk/src/app/risk_uc.cpp), [risk.proto](../../contracts/proto/fob/risk/v1/risk.proto) | П-13 | нет полнокапитального SELL из seed |
| T-CEA-204 | `committed` в PG вместо cooldown; снятие по `execution.reports` | [risk_uc.cpp:1156](../../cpp/risk/src/app/risk_uc.cpp) | П-10 | регресс двойной отправки зелёный; cooldown удалён |
| T-CEA-205 | Инварианты И-Т1…И-Т6 как стадия конвейера (до эмиссии) | [matching_loop.cpp:904](../../cpp/matching/src/app/matching_loop.cpp) | П-9 | нарушение → такт FAILED + `risk.alerts` |

## Этап 3 — исполнение (~1–2 нед) — ADR-057

| ID | Задача | Файлы | П | Acceptance |
| --- | --- | --- | --- | --- |
| T-CEA-301 | Форматтер заявок из потоков агентов (площадка из агента, не `ccy+"/"+num`) | [risk_uc.cpp:1148-1180](../../cpp/risk/src/app/risk_uc.cpp) | П-11 | venue берётся из агента; `PreHedgeCheck`=И-Т4+И-Т6 |
| T-CEA-302 | Три зоны точности заявки (пассив/IOC/рынок) из `Z_a` | risk | §A7 | тип заявки по экспозиции |

## Этап 4 — трасса и фронт (~2 нед) — ADR-059

| ID | Задача | Файлы | П | Acceptance |
| --- | --- | --- | --- | --- |
| T-CEA-401 | CH `clearing_trace` + топик `matching.clearing_trace` + писатель | [clickhouse/init.sql](../../infra/clickhouse/init.sql), [create_topics.sh](../../infra/kafka/create_topics.sh), matching | П-14 | одна запись/такт |
| T-CEA-402 | BFF эндпоинты `/api/clearing/{trace,agents,node-balances}` | [server.js:7103+](../../frontend/api/server.js) | П-16 | JSON из CH |
| T-CEA-403 | Панели В-1…В-10 (трасса, окно, стакан, кривая агента, граф узлов, цены, позиции, остатки, заявки, PnL) | [VectorClearingLive.js](../../frontend/web/src/pages/VectorClearing/VectorClearingLive.js) | В-1..10 | рукописный SVG, фронт ничего не считает |

## Тесты

| ID | Задача | П |
| --- | --- | --- |
| T-CEA-501 | Эталон Python `agents_vc.py`/`agents_sim.py` → golden; C++↔Python до 1e-9 | Ш-7 |
| T-CEA-502 | Golden-случаи §7: разрыв<Σc → нули; без запаса → ΔP≡0; committed не снят → нет повтора | §7 |
