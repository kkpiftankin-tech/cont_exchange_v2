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

## Прогресс реализации (2026-09-11) — живьём на dev, `CE_AGENTS_ENABLED=1`

Три претензии владельца («позиция не видна / заявки не ограничены новым алгоритмом /
кривая без зоны комиссии») закрыты и проверены на dev; поправка 3 (план≠факт) закоммичена.

**Сделано и проверено:**

- **Поправка 3 (план≠факт, ADR-057) — коммит `8ecc6f49`.** `ce.position.delta` (ПЛАН) копится
  в `ledger` `ce_committed_` (in-flight), НЕ в house. house/venue двигают только
  `execution.reports` (ФАКТ). Стоячая позиция (`GetExchangeBalances`/`ComputeExchangeNopLocked`) =
  только ФАКТ → двойной счёт устранён. Снапшот Clearing (`GetExchangeNopHistory`) = факт + committed,
  шаг = Δ плана. Декремент committed на филле (BUY −qty / SELL +qty) — обнуляется в такте.
  Проверка: BTC `own=10` (сид, без плана), `venue`=факт от филлов, `committed=0` стабильно,
  `snapshot_after == treasury_nop`. Файлы: [ledger_uc.hpp](../../cpp/ledger/src/app/ledger_uc.hpp),
  [ledger_uc.cpp](../../cpp/ledger/src/app/ledger_uc.cpp). Расширяет T-CEA-201 (без встречной ноги
  house; committed вместо house-факта).
- **Позиция видна (претензия 1).** treasury показывает `venue`=исполненная CE-позиция.
- **Заявки только из нового алгоритма (претензия 2).** Старый F-18/net-hedge путь заявок погашен
  (`CE_AGENTS_ENABLED`), все venue-заявки — из §A7 `ProjectOrders` (intent_id `…|ce|asset@venue`).
  Мёртвая зона `c` ограничивает эмиссию: при `|σ−σ*|≤c` поток `f=0` (тихие такты `deltas:0`).
  Позиция ограничена/осциллирует (BTC ~29…60), не растёт безгранично.
- **Зона комиссии/бездействия на кривой (претензия 3) — коммит `a8fc8b57`.** BFF
  `/vector-clearing/curve` считает `c=taker_fee_pm+½·spread_pm` (та же формула, что
  `market_data` BuildQuoteAgent) → `deadZonePm/deadLow/deadHigh + ceCurve`; страница
  рисует полосу зоны + CE-полилинию. Соответствует T-CEA-403 (панель кривой агента, частично).

**Демо-рычаг:** `SIM_VENUE_BIAS_PM_okx=20` (env, только dev, коммит `b152a150`) держит
дивергенцию > c, чтобы заявки шли и была видимая позиция. Убрать после демо.

**Отложено (не закрыто):** T-CEA-202 (PG `node_balances`/`agent_state`), T-CEA-203/204
(risk `Z_a`/committed в PG вместо cooldown), T-CEA-302 (три зоны точности заявки),
Этап 4 (clearing_trace CH+топик), полная сверка committed↔`execution.venue` (A8),
исполнение transfer-ног.

---

## Этап 0 — реальный остаток (не блокирует ничего, ~1–2 дня)

**Итог аудита (2026-09-10):** премисы T-CEA-001/002 про «502» оказались устаревшими
(паттерн CN) — на живой БД всё уже есть, `/detail` = HTTP 200. Работа свелась к
completeness/подготовке. T-CEA-003/004 — реальные фиксы.

| ID | Задача | Статус | Файлы |
| --- | --- | --- | --- |
| T-CEA-001 | ✅ **уже жило**: writer добавляет 9 колонок рантаймом, `/detail` 200 (17 сегментов). Фикс: полный fresh-DB CREATE (+5 колонок в статическом DDL) | done | [clickhouse/init.sql:501](../../infra/clickhouse/init.sql) |
| T-CEA-002 | ✅ **не баг**: BFF читает из конфига только window/stale (существуют). +5 колонок добавлены в CREATE как подготовка к Этапу 1 (для живой БД — ALTER) | done (prep) | [postgres/init.sql:942](../../infra/postgres/init.sql) |
| T-CEA-003 | ✅ реальный дрейф: `sync-proto.sh` (import-closed) регенерирует копию из `contracts/`; исправил 11 файлов + подтянул `marketdata_service.proto`; 19/19 protoc OK | done | [sync-proto.sh](../../frontend/api/sync-proto.sh), [Dockerfile](../../frontend/api/Dockerfile) |
| T-CEA-004 | ✅ хук сканирует весь payload (не только `prompt`) + debug-лог; self-test + интеграция зелёные | done | [auto-archive-attachments.py](../../tools/auto-archive-attachments.py) |

## Этап 1 — узлы и двусторонние агенты (~2–3 нед) — ADR-055/056

**Срез 1 (готов, 2026-09-10):** чистое клиринговое ядро
[`ce_agent_clearing.hpp`](../../cpp/matching/src/domain/ce_agent_clearing.hpp) (граф узлов,
единая кривая с мёртвой зоной, active-set Newton, снятие Wx=0 через свободные узлы книги) +
[GTest](../../cpp/matching/tests/domain/ce_agent_clearing_test.cpp) `matching_ce_clearing_test`
— сверка с Python-эталоном **до 1e-6** (потоки/позиция/PnL, инварианты V-CEA-002/003,
абляции G-CEA-004/005). **Срез 2:** развёртки B (полоса запаса → арбитраж/позиция) и C
(смещение марки → линейный дрейф позиции, V-CEA-005) — тоже 1e-6. Локально зелёный.
Осталось: 400-тактовая симуляция D (нужен mids-fixture + Z/committed — Срез с risk),
обвязка market_data (агенты/базис) и matching money-path (T-CEA-103/104/105/107).

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
