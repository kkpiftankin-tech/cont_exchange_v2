# Implementation Tasks: F-06 Positions / PnL / Margin

## Source Artifacts

- Feature: [F-06 Positions / PnL / Margin](../02-system/features/F-06-positions-pnl-margin/)
- Use Case: [UC-F06-01](../02-system/use-cases/UC-F06-01-show-positions/use-case.md)
- System Sequence: [SEQ-UC-F06-01-system](../02-system/use-cases/UC-F06-01-show-positions/sequences/SEQ-UC-F06-01-system.md)
- Service Sequence: [SEQ-F06-UC-F06-01-services](../05-components/sequences/SEQ-F06-UC-F06-01-services.md)
- Component Sequences (L2 🐟):
  - [SEQ-LEDGER-001-get-positions](../05-components/ledger/sequences/SEQ-LEDGER-001-get-positions.md) — чтение/сборка GetPositions + unrealized PnL
  - [SEQ-LEDGER-002-apply-fill-position-update](../05-components/ledger/sequences/SEQ-LEDGER-002-apply-fill-position-update.md) — apply fill, flip, mark-to-market
  - [SEQ-RISK-001-build-risk-snapshot](../05-components/risk-manager/sequences/SEQ-RISK-001-build-risk-snapshot.md) — margin, margin-call, persist + alert
- Contracts:
  - [`fob.ledger.v1.LedgerService.GetPositions`](../06-api/grpc/ledger-get-positions.md)
  - [`fob.ledger.v1.LedgerService.GetBalances`](../06-api/grpc/ledger-get-balances.md)
  - [`fob.ledger.v1.LedgerService.ApplyBatchResult`](../06-api/grpc/ledger-apply-batch-result.md)
  - [`fob.risk.v1.RiskService.OnBatchResult`](../06-api/grpc/risk-on-batch-result.md), [`GetRiskSnapshot`](../06-api/grpc/risk-get-risk-snapshot.md)
  - [batch.outputs](../06-api/messaging/batch-outputs.md), [risk.alerts](../06-api/messaging/risk-alerts.md)
  - REST `GET /v1/positions` (alias `/api/v1/positions`) — см. [rest/README.md](../06-api/rest/README.md)
- Data: [`accounts`](../07-data/oltp-schema.md#таблица-accounts), [`positions`](../07-data/oltp-schema.md#таблица-positions), [`risk_limits`](../07-data/oltp-schema.md#таблица-risk_limits), [`risk_snapshots`](../07-data/oltp-schema.md#таблица-risk_snapshots)
- IN-001 fragments: IN-001-FR-027, IN-001-FR-028 (FR-LEDGER-001/002, §6)
- Требования: F6-1..F6-10 (см. ниже «Маппинг F6 → задача»).

## Preconditions

- [x] Feature exists
- [x] Use case exists
- [x] System-level sequence exists
- [x] Service-level sequence exists
- [x] Component-level (L2) sequences exist
- [x] Contracts exist (gRPC + Kafka topics)
- [x] Data objects exist (DDL в oltp-schema)
- [x] Acceptance criteria exist (feature.yaml)

## Требования F6-1..F6-10

| ID | Требование |
| --- | --- |
| F6-1 | `positions`/`accounts` обновляются при каждом `FillEvent` (apply `BatchResult`). |
| F6-2 | После каждого батча Risk строит `RiskSnapshot` (margin, leverage, free/maintenance). |
| F6-3 | gRPC `LedgerService.GetPositions` отдаёт позиции + unrealized PnL по mark price. |
| F6-4 | gateway `GET /v1/positions` агрегирует ledger + risk и возвращает клиенту. |
| F6-5 | WebSocket push: при новом `RiskSnapshot`/обновлении позиций клиент получает апдейт. |
| F6-6 | UI: таблица позиций (symbol, side, qty, avg entry, mark, uPnL, rPnL). |
| F6-7 | UI: блок маржи (free/used collateral, initial/maintenance margin, leverage) + подсветка margin call. |
| F6-8 | Multi-leg / портфельные позиции отображаются корректно (агрегация по legs). |
| F6-9 | Polling fallback, если WS недоступен. |
| F6-10 | Все денежные величины — `Decimal`; PnL детерминирован и покрыт тестами. |

## Волны реализации

```text
Волна 0: proto-контракты      T-F06-001..002
Волна 1: DDL БД               T-F06-010..011
Волна 2: ledger               T-F06-020..023
Волна 3: risk                 T-F06-030..032
Волна 4: gateway              T-F06-040..041
Волна 5: frontend             T-F06-050..052
Волна 6: тесты                T-F06-060..062
```

---

## Волна 0 — proto-контракты

> ⚠️ `.proto` и contract-docs в `docs/06-api/grpc/` ведёт отдельный поток (contract-first PR). Здесь задачи фиксируют ожидаемую поверхность для downstream-волн; код-волны зависят от их merge.

### T-F06-001. Proto: `GetPositions` в `LedgerService`

Target:

- [contracts/proto/fob/ledger/v1/ledger.proto](../../contracts/proto/fob/ledger/v1/ledger.proto): RPC `GetPositions(GetPositionsRequest) returns (GetPositionsResponse)`.
- `Position{symbol, side, quantity, avg_entry_price, mark_price, unrealized_pnl, realized_pnl}` (все money — `fob.common.v1.Decimal`).
- `GetPositionsResponse{positions[], accounts[], as_of, bool mark_stale}`.

Acceptance:

- proto-build зелёный; `python3 tools/proto-contract-auditor/check_proto_map.py` проходит;
- contract-doc [ledger-get-positions.md](../06-api/grpc/ledger-get-positions.md) переведён из `planned` в `proto-defined`.

Зависимости: нет.

### T-F06-002. Proto: `OnBatchResult` / `GetRiskSnapshot` в `RiskService`

Target:

- [contracts/proto/fob/risk/v1/risk.proto](../../contracts/proto/fob/risk/v1/risk.proto): `GetRiskSnapshot(user_id)` → `RiskSnapshot{free_collateral, reserved_collateral, initial_margin, maintenance_margin, leverage, risk_flags}`.
- `OnBatchResult` принимает `batch_id` + список затронутых пользователей.

Acceptance:

- proto-build зелёный;
- contract-docs [risk-get-risk-snapshot.md](../06-api/grpc/risk-get-risk-snapshot.md), [risk-on-batch-result.md](../06-api/grpc/risk-on-batch-result.md) → `proto-defined`.

Зависимости: нет.

---

## Волна 1 — DDL БД (PostgreSQL)

### T-F06-010. Миграция `accounts` + `positions`

Target:

- `infra/postgres/migrations/00xx_accounts_positions.sql` по DDL из [oltp-schema.md](../07-data/oltp-schema.md#таблица-accounts) и [`positions`](../07-data/oltp-schema.md#таблица-positions).
- `accounts`: `UNIQUE (user_id, asset)`, поля `free_balance/reserved_balance/venue_allocated/pending_transfer`.
- `positions`: `UNIQUE (user_id, symbol)`, `side ENUM('long','short','flat')`, `unrealized_pnl`, `realized_pnl`.
- Сидинг demo-баланса `demo-user` (10000 USDT, 1 BTC) — перенос из in-memory конструктора ledger.

Acceptance:

- миграция применяется при старте compose чисто;
- индекс/уникальность по `(user_id, asset)` и `(user_id, symbol)` присутствуют.

Зависимости: нет.

### T-F06-011. Миграция `risk_limits` + `risk_snapshots`

Target:

- `infra/postgres/migrations/00xx_risk_limits_snapshots.sql` по [`risk_limits`](../07-data/oltp-schema.md#таблица-risk_limits) и [`risk_snapshots`](../07-data/oltp-schema.md#таблица-risk_snapshots).
- `risk_snapshots`: индекс `(entity_id, timestamp DESC)`, `risk_flags JSONB`.
- Сидинг default `risk_limits` на роль (`demo`, `client`) с `max_leverage`.

Acceptance:

- миграция чистая;
- default-лимиты доступны для расчёта margin.

Зависимости: T-F06-010 (общий init-порядок). Может пересекаться с F-07 T-F07-001 (`risk_limits`) — переиспользовать существующую миграцию, не дублировать.

---

## Волна 2 — ledger

### T-F06-020. `PositionRepository` + `AccountRepository` (PostgreSQL)

Target:

- `cpp/ledger/src/infra/postgres/postgres_position_repository.{hpp,cpp}` — `ListByUser`, `LoadForUpdate(SELECT … FOR UPDATE)`, `Upsert(ON CONFLICT (user_id,symbol))`.
- `cpp/ledger/src/infra/postgres/postgres_account_repository.{hpp,cpp}` — `ListByUser`, `SettleFill`.
- Вынести in-memory state ledger за порт-интерфейсы (`IPositionRepository`, `IAccountRepository`).

Acceptance:

- балансы/позиции переживают рестарт (закрывает known issue `no-persistence`);
- integration-тест с тестовой PG (I1).

Зависимости: T-F06-010.

### T-F06-021. `GetPositions` use-case + gRPC handler

Target:

- Реализовать [SEQ-LEDGER-001](../05-components/ledger/sequences/SEQ-LEDGER-001-get-positions.md): `LedgerUseCases::GetPositions(user_id)` — чтение позиций+аккаунтов, пересчёт `unrealized_pnl` по mark price (`MarkPriceProvider` из market-data), сборка ответа с `as_of`/`mark_stale`.
- gRPC override `GetPositions` в `grpc_ledger_service.cpp`.
- `PnlCalculator::ComputeUnrealized` (long: `(mark-entry)*qty`; short: `(entry-mark)*qty`).

Acceptance (F6-3, F6-10):

- ответ содержит позиции с корректным uPnL; stale mark → `mark_stale=true`, last persisted PnL;
- unit-тесты U1–U3.

Зависимости: T-F06-001, T-F06-020.

### T-F06-022. `ApplyBatchResult`: persistence + flip + mark-to-market

Target:

- Реализовать [SEQ-LEDGER-002](../05-components/ledger/sequences/SEQ-LEDGER-002-apply-fill-position-update.md): на каждый `FillEvent` — idempotency `(batch_id,order_id,fill_id)`, `SettleFill` по accounts, апдейт позиции (increase / reduce / **flip**), `realized_pnl` на закрытом объёме, пересчёт `avg_entry_price` и `unrealized_pnl`, всё в одной TX.
- Offset-commit после `COMMIT`.

Acceptance (F6-1, F6-10):

- `positions`/`accounts` корректно обновляются после каждого `BatchResult`;
- flip через ноль фиксирует realized только на закрытом объёме;
- двойная доставка не плодит балансовых операций (idempotent);
- unit-тесты U4–U7, integration I2.

Зависимости: T-F06-020.

### T-F06-023. Fix known issues: buy-reserve-leak + scale-inflation

Target:

- При FILLED BUY освобождать разницу `reserved - executed_notional` (см. F-04 known issue, order-flow overview).
- Нормализовать scale `Decimal` к каноническому per-currency перед записью (known issue `scale-inflation`).

Acceptance:

- reserved не «протекает» после полного исполнения BUY;
- scale записываемых величин стабилен; unit-тест U8.

Зависимости: T-F06-022.

---

## Волна 3 — risk

### T-F06-030. risk consumer `batch.outputs` + `OnBatchResult`

Target:

- `cpp/risk/src/infra/kafka_consumers.cpp` — consumer `batch.outputs`, извлечение затронутых `user_id` из fills, вызов `RiskUseCases::OnBatchResult`.
- Idempotency по `batch_id`.

Acceptance:

- каждый батч триггерит ровно один проход по затронутым пользователям;
- offset-commit после успешной обработки.

Зависимости: T-F06-002, T-F06-022 (positions уже обновлены ledger'ом до построения снапшота — порядок гарантируется отдельными consumer-группами; снапшот читает актуальное состояние из PG).

### T-F06-031. `buildRiskSnapshot`: margin + margin-call + persist

Target:

- Реализовать [SEQ-RISK-001](../05-components/risk-manager/sequences/SEQ-RISK-001-build-risk-snapshot.md): чтение positions/accounts/risk_limits, `MarginCalculator` (`initial_margin = notional/max_leverage`, `maintenance_margin`, `free_collateral`), evaluate `margin_call`/`liquidation`/`throttled`, `INSERT risk_snapshots`.
- Заменить placeholder `margin = qty*price*0.1` (known issue `no-margin-calculation`).
- `RiskSnapshotRepository` (PostgreSQL).

Acceptance (F6-2, F6-7):

- после батча в `risk_snapshots` появляется строка с корректной маржой и `risk_flags`;
- margin call определяется при `free_collateral < maintenance_margin`;
- unit-тесты U9–U10.

Зависимости: T-F06-011, T-F06-030.

### T-F06-032. `RiskAlert` margin-call в `risk.alerts` + `GetRiskSnapshot`

Target:

- При `margin_call`/`liquidation` публиковать `RiskAlert{type, severity, user_id, batch_id}` в `risk.alerts` (после `INSERT` снапшота).
- gRPC `GetRiskSnapshot(user_id)` — отдаёт последний снапшот для gateway.

Acceptance:

- alert виден в Observability; нет alert без персистентного снапшота;
- `GetRiskSnapshot` отдаёт актуальную маржу; integration I3.

Зависимости: T-F06-031.

---

## Волна 4 — gateway

### T-F06-040. `GET /v1/positions` (агрегация ledger + risk)

Target:

- `cpp/gateway/src/transport/` — handler `GET /v1/positions` (alias `/api/v1/positions`): параллельные gRPC `LedgerService.GetPositions` + `RiskService.GetRiskSnapshot`, объединение в JSON (positions[], accounts[], margin-блок).
- Авторизация по `user_id` из сессии.

Acceptance (F6-4):

- ответ содержит позиции, балансы и margin-блок;
- частичная деградация: если risk недоступен — позиции отдаются с `margin: null` (WARN);
- integration I4.

Зависимости: T-F06-021, T-F06-032.

### T-F06-041. WebSocket push позиций/маржи

Target:

- WS-эндпоинт `GET /v1/stream?topics=positions` (расширение ws-gateway из F-04 T-F04-160): подписка на `risk.alerts` + новые снапшоты, push клиенту обновлений позиций/маржи.
- Heartbeat, graceful close.

Acceptance (F6-5):

- при новом `RiskSnapshot`/margin-call клиент получает push ≤ 1 s;
- 100 параллельных соединений стабильны;
- integration I5.

Зависимости: T-F06-040, T-F06-032.

---

## Волна 5 — frontend

### T-F06-050. Страница позиций (таблица)

Target:

- Страница «Позиции»: таблица `symbol | side | qty | avg entry | mark | uPnL | rPnL`, сортировка/обновление.
- Источник — `GET /v1/positions`.

Acceptance (F6-6):

- таблица отображает все открытые позиции с корректными PnL;
- `flat`-позиции скрываются или помечаются.

Зависимости: T-F06-040.

### T-F06-051. Блок маржи + подсветка margin call

Target:

- Виджет маржи: free/used collateral, initial/maintenance margin, leverage.
- Подсветка состояния `margin_call` (warning) / `liquidation` (critical) по `risk_flags`.

Acceptance (F6-7):

- значения совпадают с `GetRiskSnapshot`;
- при margin call блок визуально подсвечивается.

Зависимости: T-F06-050.

### T-F06-052. Multi-leg агрегация + polling fallback

Target:

- Корректное отображение портфельных / multi-leg позиций: агрегация по legs (см. `flow_order_legs`).
- WS-подписка на обновления; **polling fallback** (`GET /v1/positions` каждые N сек), если WS недоступен.

Acceptance (F6-8, F6-9):

- multi-leg позиция показана как агрегат + раскрытие по legs;
- при разрыве WS UI переключается на polling без потери данных;
- integration I6 (WS down → polling).

Зависимости: T-F06-050, T-F06-041.

---

## Волна 6 — тесты

### T-F06-060. Unit-тесты U1–U10

Target — `cpp/ledger/tests/` и `cpp/risk/tests/`:

| # | Что проверяет |
| --- | --- |
| U1 | uPnL long: `(mark-entry)*qty` |
| U2 | uPnL short: `(entry-mark)*qty` |
| U3 | uPnL при `side=flat`/`qty=0` → 0 |
| U4 | increase: weighted `avg_entry_price`, realized без изменений |
| U5 | reduce частичный: realized на закрытом объёме |
| U6 | close полный: `side=flat`, `quantity=0` |
| U7 | flip через ноль: realized только на закрытом, новая нога по fill_price |
| U8 | scale-инвариант Decimal после серии операций |
| U9 | margin: `initial_margin = notional/max_leverage` |
| U10 | margin call: `free_collateral < maintenance_margin` → флаг |

Acceptance:

- все 10 зелёные на CI; PnL детерминирован (F6-10).

Зависимости: T-F06-021, T-F06-022, T-F06-031.

### T-F06-061. Integration-тесты I1–I6

Target:

| # | Сценарий |
| --- | --- |
| I1 | persistence: рестарт ledger → балансы/позиции сохранены |
| I2 | full cycle: FlowOrder → batch.outputs → ledger apply → positions обновлены |
| I3 | batch.outputs → risk → `risk_snapshots` строка + `GetRiskSnapshot` |
| I4 | `GET /v1/positions` агрегирует ledger+risk |
| I5 | WS push при новом снапшоте / margin call |
| I6 | WS down → frontend polling fallback |

Acceptance:

- все 6 проходят на docker-compose стенде (nightly).

Зависимости: соответствующие код-волны.

### T-F06-062. Нагрузочные / SLA-тесты

Target — `tests/performance/f06/`:

- `GET /v1/positions` p95 latency при N пользователей с M позициями.
- Throughput `ApplyBatchResult` при потоке fills (apply rate).
- `buildRiskSnapshot` p95 на батч с K затронутых пользователей.

Acceptance:

- `GET /v1/positions` p95 ≤ 150 ms;
- apply fills не отстаёт (consumer lag ≤ 5 s под целевой нагрузкой);
- `buildRiskSnapshot` укладывается в batch_interval.

Зависимости: T-F06-021, T-F06-022, T-F06-031.

---

## Out of scope

- Ликвидации и принудительное закрытие позиций (F-08) — здесь только `liquidation`-флаг в снапшоте.
- Pre-trade проверки лимитов (F-07) — переиспользуют `risk_limits`/`accounts`, но реализуются в своей фиче.
- Deposit/withdraw движение средств (F-14).
- ClickHouse-история позиций/снапшотов (OLAP) — отдельный поток аналитики.

## Маппинг F6 → задача

| Требование | Closing tasks |
| --- | --- |
| F6-1 | T-F06-022 |
| F6-2 | T-F06-030, T-F06-031 |
| F6-3 | T-F06-001, T-F06-021 |
| F6-4 | T-F06-040 |
| F6-5 | T-F06-041 |
| F6-6 | T-F06-050 |
| F6-7 | T-F06-031, T-F06-051 |
| F6-8 | T-F06-052 |
| F6-9 | T-F06-052 |
| F6-10 | T-F06-021, T-F06-022, T-F06-060 |

## Оценка трудозатрат (порядок величин)

| Волна | Задачи | Усилия |
| --- | --- | --- |
| 0: proto | T-F06-001..002 | 1 день |
| 1: DDL | T-F06-010..011 | 1 день |
| 2: ledger | T-F06-020..023 | 5–7 дней |
| 3: risk | T-F06-030..032 | 4–6 дней |
| 4: gateway | T-F06-040..041 | 3–4 дня |
| 5: frontend | T-F06-050..052 | 4–6 дней |
| 6: тесты | T-F06-060..062 | 4–6 дней |

Итого: **22–31 рабочий день** для одного разработчика; ~12–15 календарных при двух.

## Next

После завершения T-F06-001..062 — обновить статус F-06 в [`coverage-matrix.md`](../traceability/coverage-matrix.md) до `complete` и снять known issues `no-persistence`, `no-position-tracking`, `no-pnl`, `no-margin-calculation`, `buy-reserve-leak`, `scale-inflation` в [feature.yaml](../02-system/features/F-06-positions-pnl-margin/feature.yaml).

## Follow-up (post-MVP)

Тикеты следующего этапа F-06, оформленные ADR-ами. Каждый ссылается на соответствующее
архитектурное решение; реализация — отдельными PR после волн 0–6.

### T-F06-070. Зеркалирование резервов в `accounts` (mirror → single PG source)

- **ADR:** [ADR-044](../03-architecture/adr/ADR-044-ledger-balance-source-of-truth.md)
- `ReserveFunds`/`ReleaseFunds` зеркалятся в `accounts` атомарными PG-проводками
  (`free↔reserved`), идемпотентно по `reservation_id`; убрать маскирующий
  `GREATEST(0, …)` после стабилизации; in-memory → read-кэш, гидрация из PG на старте.
- Закрывает known issue `reserve-not-mirrored-to-accounts` (+ часть `no-persistence`).
- Зависимость: T-F06-071 (PG-пул) как предусловие для PG-записи в горячем пути reserve.

### T-F06-071. Пул соединений PostgreSQL (`cex::common::PgConnectionPool`)

- **ADR:** [ADR-045](../03-architecture/adr/ADR-045-pg-connection-pooling.md)
- Реализовать `PgConnectionPool` в `cpp/common` (RAII-handle, размер через env,
  reconnect, graceful shutdown); подключить в ledger и risk вместо per-call
  `pqxx::connection`. Σ(размеров пулов) ≤ `max_connections`.
- Закрывает known issue `pg-per-call-connection`; снимает ~50% отказов при 500 conc.
- Связано с T-F06-062 (нагрузочные SLA).

### T-F06-072. WS-push позиций через топик `positions.update`

- **ADR:** [ADR-046](../03-architecture/adr/ADR-046-positions-update-topic.md)
- ledger публикует `positions.update` (key=`user_id`, payload `{user_id, batch_id, ts}`)
  после `ApplyBatchResult`; gateway (ws-gateway) консьюмит и реагрегирует снимок через
  `positions_handler`. Регистрация топика в `infra/kafka/create_topics.sh` — кодовый
  агент (см. [positions.update.md](../06-api/messaging/positions.update.md)).
- Закрывает known issue `no-positions-update-push-signal`; реализует F6-5 (расширяет T-F06-041).

### T-F06-073. Идемпотентность снапшотов / реагрегации

- **ADR:** [ADR-046](../03-architecture/adr/ADR-046-positions-update-topic.md) (at-least-once),
  [ADR-020](../03-architecture/adr/ADR-020-event-ordering-idempotency.md)
- gateway-потребитель `positions.update` идемпотентен по `batch_id` (повторная/устаревшая
  реагрегация безвредна; дедуп уже обработанных `batch_id`); ledger дедуплицирует
  затронутых `user_id` внутри батча (одно сообщение на пользователя).
- Гарантирует отсутствие лишних push/рассинхрона при дубликатах сигнала.

### T-F06-074. Multi-leg атрибуция позиций (per-leg)

- **ADR:** связь F-06 → F-09 (`relatedFeatures`), per-leg атрибуция combo/портфельных заявок.
- Корректная агрегация и раскрытие позиций/PnL по ногам (`flow_order_legs` /
  `combo_order_legs`); реализует F6-8 (расширяет T-F06-052).
- Зависимость: модель F-09 ([ADR-031](../03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md),
  [ADR-032](../03-architecture/adr/ADR-032-parent-child-order-model.md)).

### T-F06-075. FK `positions/accounts.user_id → users` (зависимость F-06 → F-01)

- **ADR:** связь F-06 → F-01 (`relatedFeatures`), реестр пользователей.
- Добавить внешний ключ `user_id` из `accounts`/`positions` на таблицу `users` (F-01)
  после появления реестра пользователей; до этого `user_id` — строковый идентификатор
  без FK (demo-user).
- Зависимость: F-01 (таблица `users`).

## Source Fragments

- IN-001-FR-027, IN-001-FR-028 (FR-LEDGER-001/002, §6 — feature baseline)
