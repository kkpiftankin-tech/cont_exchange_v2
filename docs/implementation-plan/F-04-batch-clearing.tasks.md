# Implementation Tasks: F-04 Batch Clearing

## Progress (current series, 2026-05)

F-04 имплементируется серией PR из ветки `origin/dev`:

| PR                                                                                            | Что сделано                                                                                                                                          | Покрывает задачи         |
| --------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------ |
| [#6 build fixes](https://github.com/kkpiftankin-tech/cont_exchange_v2/pull/6) merged          | Dockerfile.service: slim Boost, fixed ENTRYPOINT; rpk healthcheck; idempotent topics-init.                                                           | prerequisite             |
| [#7 proto contracts](https://github.com/kkpiftankin-tech/cont_exchange_v2/pull/7) merged      | `batch.proto` LiquidityProvenance, `fill_event.proto`, `batch_outputs.proto`, `solver.proto`.                                                        | T-F04-001 (контракт)     |
| [#8 cpp/common update](https://github.com/kkpiftankin-tech/cont_exchange_v2/pull/8) merged    | `Decimal::zero()`, `Defer`, `replay_kafka`, new `ConsumerConfig` fields. Foundation.                                                                  | prerequisite             |
| #N feat/f04-matching (current)                                                                | Eigen3 Sparse Cholesky solver, `RunBatchUseCase`, `PostgresFlowOrderRepository`, `PostgresSolverConfigRepository`, `MarketDataClient`, `BatchOutputsProducer`, U1-U10 tests, full-cycle+SLA integration tests. | T-F04-001, T-F04-002 (код), T-F04-005, T-F04-006 |
| `feat/f04-postgres` (next, step 3)                                                            | DDL `flow_orders`, `solver_config` в init.sql; postgres сервис в docker-compose.dev.yml.                                                             | T-F04-002 (БД), T-F04-007 |
| `feat/f04-clickhouse` (next, step 4)                                                          | clickhouse сервис; ingestion `batchresults`, `fills`.                                                                                                | T-F04-003                |

Подробности по статусу AC-1..AC-15 и unit-тестам U1-U10 — в [acceptance-criteria.md](../02-system/features/F-04-batch-clearing/acceptance-criteria.md).

## Source Artifacts

- Feature: [F-04 Batch Clearing](../02-system/features/F-04-batch-clearing/)
- Use Case: [UC-F04-01](../02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md)
- System Sequence: [SEQ-UC-F04-01-system](../02-system/use-cases/UC-F04-01-run-batch-clearing/sequences/SEQ-UC-F04-01-system.md)
- Service Sequence: [SEQ-F04-UC-F04-01-services](../05-components/sequences/SEQ-F04-UC-F04-01-services.md)
- Contracts: [batch.outputs](../06-api/messaging/batch-outputs.md), [orders.normalized](../06-api/messaging/orders-normalized.md), `fob.matching.v1.BatchResult`
- Data: [`flow_orders`](../07-data/oltp-schema.md#таблица-flow_orders), [`solver_config`](../07-data/oltp-schema.md#таблица-solver_config), [`fills`](../07-data/olap-schema.md#таблица-fills), [`batch_results`](../07-data/olap-schema.md#таблица-batch_results)
- IN-001 fragments: IN-001-FR-016 (FR-CLEAR-001/002), IN-001-FR-019, IN-001-FR-028

## Preconditions

- [x] Feature exists
- [x] Use case exists
- [x] System-level sequence exists
- [x] Service-level sequence exists
- [x] Contracts exist (Kafka topics, BatchResult proto)
- [x] Data objects exist (DDL в oltp/olap-schema)
- [x] Acceptance criteria exist

## Tasks

### T-F04-001. Solver: реальный алгоритм клиринга

Target:

- Заменить MVP-симулятор в `cpp/matching/src/domain/solver.{hpp,cpp}` на реальный solver задачи равновесия.
- Поддержка ограничений по `p_low/p_high`, `q_rate`, `q_max`.
- Выход: `clear_prices`, `executed_rates`, `fills`, `BatchDiagnostics`.

Acceptance:

- residual norm ≤ `solver_config.tolerance`;
- solve time p50 ≤ 200 ms, p95 ≤ 500 ms на ~1000 активных ордеров;
- replay даёт идентичный результат при том же `config_version`.

### T-F04-002. Чтение `solver_config` из PostgreSQL

Target:

- Загрузка active config перед каждым batch (с кэшем + invalidation).
- Поддержка hot-reload без рестарта.

Acceptance:

- изменение `is_active` config'а применяется ≤ batch_interval.

### T-F04-003. ClickHouse ingestion `fills` / `batch_results`

Target:

- Kafka engine table + materialized view → MergeTree.
- Партиционирование `toYYYYMMDD(timestamp)`.

Acceptance:

- consumer lag ≤ 5 секунд на нормальной нагрузке;
- ad-hoc запросы по symbol+timestamp выполняются за p95 ≤ 1 s.

### T-F04-004. Idempotent apply в Ledger

Target:

- Ledger consumer `batch.outputs` применяет fills атомарно по ключу `(batch_id, order_id, fill_id)`.

Acceptance:

- повторная доставка не порождает повторных балансовых операций;
- интеграционный тест двойной доставки зелёный.

### T-F04-005. Observability метрики solver'а

Target:

- Экспорт метрик: solve_time, residual_norm, num_active_orders, consumer lag.
- Алерты при p95 solve_time > 500 ms 5 минут подряд.

Acceptance:

- метрики в Prometheus формате;
- alert routing настроен (см. F-17).

### T-F04-006. Replay-тесты

Target:

- Сценарии в `tests/backtest/`: записанная история → ожидаемый `BatchResult`.
- Эталонные fixtures зафиксированы.

Acceptance:

- все scenario fixtures стабильно проходят;
- diff с эталоном — нулевой при том же `config_version`.

## Out of scope

- Combo / portfolio matching (F-09).
- LOB→FOB конвертация (F-11).
- Execution hedge (F-12).

## Definition of Done (IN-003)

Чек-лист DoD из IN-003 §6 — каждое условие соответствует одной из задач T-F04-XXX выше.

- [ ] Реализован `solveBatch(...)` в Matching Backend (FOB Core); возвращает `BatchResult` и `FillEvent` в формате entities (см. T-F04-001).
- [ ] Пройден набор юнит-тестов солвера (U1–U10) — см. [F-04 test plan](../10-testing/features/F-04-test-plan.md) (T-F04-007).
- [ ] Matching Backend читает активные FlowOrder из PostgreSQL (`flow_orders WHERE status='active'` + окно времени) — T-F04-002.
- [ ] Matching Backend запрашивает reference prices у Market Data Service и использует их в `solveBatch` — T-F04-001, T-F04-002.
- [ ] После батча публикуются события в Kafka: `batch.outputs` (текущий MVP — single topic; planned split на `batch.outputs` + `fills` — см. Conflict Note C-1 в [topics.md](../06-api/messaging/topics.md)).
- [ ] ClickHouse принимает и сохраняет `batch_results` и `fills` через Kafka engine — T-F04-003.
- [ ] Пройден интеграционный тест «полный цикл»: FlowOrder → RunBatch → Kafka → ClickHouse, с проверкой инвариантов (баланс объёмов, диапазон цен) — T-F04-007.
- [ ] Пройден SLA-тест: p95/p99 `solve_time_ms` для целевых размеров батча укладываются в пороги из [NFR-EXEC-002 таблицы](../02-system/non-functional-requirements.md#nfr-exec-002-solver-latency) — T-F04-005, T-F04-007.
- [ ] В UI доступен экран «Диагностика клиринга» с отображением `batch_id`, `clear_prices`, `executed_rates`, `residual_norm`, `solve_time_ms` и списком FillEvent.
- [ ] Настроены метрики и алерты по `solve_time_ms` и `residual_norm`; есть короткий операторский чек-лист «что делать при отклонениях» — T-F04-005.

### Дополнительные задачи из IN-003

#### T-F04-007. Тестовая поверхность (юнит + интеграция + manual)

Target:

- Юнит-тесты solver (U1–U10) — `cpp/matching/tests/` или эквивалент.
- Интеграционный «полный цикл» — `tests/integration/f04/`.
- SLA-тест по размеру батча — nightly job.
- Ручные сценарии (Функциональные / Диагностические / Негативные) задокументированы.

Acceptance:

- все юнит-тесты U1–U10 зелёные;
- интеграционный тест end-to-end зелёный на docker-compose стенде;
- nightly SLA-job стабильно проходит на N = 50, 200, 500, 1000.

Подробнее: [F-04 test plan](../10-testing/features/F-04-test-plan.md).

#### T-F04-008. Conflict resolution: split `batch.outputs` → `batch.outputs` + `fills` (future)

Target (future, post-MVP):

- Раздельный Kafka topic `fills` для `FillEvent` (партиционирование по `order_id` для idempotency).
- `batch.outputs` остаётся для `BatchResult` (партиционирование по `batch_id`).
- Миграция consumer'ов Ledger/Risk/Observability.

Acceptance:

- per-topic contract docs в `docs/06-api/messaging/` обновлены;
- Conflict Note C-1 в [topics.md](../06-api/messaging/topics.md) переведена в «resolved».

## Детализация по AC (code-level work breakdown)

Подробный план, чтобы закрыть каждый пункт [acceptance-criteria.md](../02-system/features/F-04-batch-clearing/acceptance-criteria.md). Группы упорядочены по зависимостям: сначала proto-схема и детерминизм (быстрые wins без новых внешних систем), затем persistence, затем гRPC-интеграции и UI.

Префикс новых задач — `T-F04-1XX`, чтобы не путать с верхнеуровневыми `T-F04-001..008`. В колонке «AC» — пункты [acceptance-criteria.md](../02-system/features/F-04-batch-clearing/acceptance-criteria.md), которые задача закрывает.

### Фаза 1 — proto и quick wins (без новых deps)

#### T-F04-101. Proto: расширить FlowFill и BatchSolverDiagnostics

AC: подготовка к AC-5, AC-13, AC-14, AC-15.

Изменить [contracts/proto/fob/matching/v1/batch.proto](../../contracts/proto/fob/matching/v1/batch.proto):

- `FlowFill` — добавить:
  - `string fill_id = 9;` (UUID, идемпотентный ключ для ledger/CH);
  - `string batch_id = 10;` (для join в CH);
  - `string liquidity_source = 11;` (`"internal"`, `"hedge:binance"`, …).
- `BatchSolverDiagnostics` — добавить:
  - `bool stopped_by_tolerance = 6;`
  - `bool stopped_by_max_iterations = 7;`
  - `bool sla_breach = 8;`
  - `bool degraded = 9;` (residual > tolerance OR sla_breach).
- Пересобрать `contracts_proto`; пройти `python3 tools/proto-contract-auditor/check_proto_map.py`.

Acceptance: proto-build зелёный, новые поля видны в `.pb.h`, существующие consumers не ломаются (поля optional).

Зависимости: нет.

#### T-F04-102. Детерминизм итерации active orders

AC: AC-13.

Файл [cpp/matching/src/app/matching_loop.cpp:160](../../cpp/matching/src/app/matching_loop.cpp).

Сейчас:

```cpp
for (auto& [oid, o] : active_) { ... }
```

на `std::unordered_map`. Заменить на детерминированный проход:

```cpp
std::vector<std::string> ids;
ids.reserve(active_.size());
for (auto& [oid, _] : active_) ids.push_back(oid);
std::sort(ids.begin(), ids.end());
for (const auto& oid : ids) { auto& o = active_[oid]; ... }
```

То же для `sym_sum` при заполнении `clear_prices` — обходить отсортированный ключ-список.

Acceptance: одинаковые входы → побитово идентичный `BatchResult` (за исключением `meta.event_id` и `batch_id`, которые UUID — их вынести в DI seed для replay-режима).

Зависимости: нет.

#### T-F04-103. Измерение solve_time_ms

AC: AC-10, AC-11.

Файл [cpp/matching/src/app/matching_loop.cpp:129](../../cpp/matching/src/app/matching_loop.cpp) (`run_one_batch`).

Заменить hardcoded `diag->set_solve_time_ms(1)`. Обернуть solve-блок (с момента построения списка active до завершения формирования `BatchResult`) `std::chrono::steady_clock`:

```cpp
const auto t0 = std::chrono::steady_clock::now();
// ... solve ...
const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - t0).count();
diag->set_solve_time_ms(static_cast<uint32_t>(dt));
```

Acceptance: `solve_time_ms` отражает реальную длительность; для пустого batch ≈ 0.

Зависимости: нет.

#### T-F04-104. Вычисление residual_norm

AC: AC-12.

Файл [cpp/matching/src/app/matching_loop.cpp](../../cpp/matching/src/app/matching_loop.cpp) (`run_one_batch`).

Для текущего MVP-симулятора определить residual как нарушение баланса buy ↔ sell по каждому символу:

```text
residual_norm = sqrt(sum_over_symbols((sum_buy_qty - sum_sell_qty)^2))
              / max(1, total_qty)
```

Хранить промежуточные `buy_qty_by_sym`/`sell_qty_by_sym`; считать после распределения fills. Записывать в `diag->set_residual_norm(value)`.

Acceptance: для симметричного buy/sell кейса residual ≤ 1e-9; для асимметричного — отражает дисбаланс.

Зависимости: T-F04-102 (детерминизм важен для повторяемости).

#### T-F04-105. SLA / convergence flags + alerts

AC: AC-14, AC-15.

Изменения:

1. [cpp/matching/src/app/matching_loop.cpp](../../cpp/matching/src/app/matching_loop.cpp): после T-F04-103/T-F04-104 — проверки и установка flags:

   ```cpp
   const double tolerance = 1e-6;          // позже — из solver_config
   const uint32_t sla_p95_ms = 500;        // позже — из NFR-EXEC-002
   diag->set_stopped_by_tolerance(residual <= tolerance);
   diag->set_sla_breach(dt > sla_p95_ms);
   diag->set_degraded(diag->stopped_by_max_iterations()
                      || !diag->stopped_by_tolerance()
                      || diag->sla_breach());
   ```

2. Если `degraded` — публиковать дополнительный `RiskAlert` в `risk.alerts`:
   - `alert_type = "SOLVER_NOT_CONVERGED"` (если `residual > tolerance`);
   - `alert_type = "SLA_BREACH"` (если `solve_time_ms > sla`);
   - severity `WARN` / `CRITICAL` по матрице.

3. Для записи в `risk.alerts` нужен `KafkaProducer` в `MatchingLoop` с топиком `risk.alerts` (отдельный, чтобы Risk Manager и Observability могли консьюмить). Добавить второй producer instance или использовать существующий с другим topic-параметром.

Acceptance: при искусственном `tolerance=1e-12, max_iterations=10` (см. U9) — генерируется `risk.alerts(SOLVER_NOT_CONVERGED)`; при намеренной задержке (sleep) — `SLA_BREACH`.

Зависимости: T-F04-101 (новые поля BatchSolverDiagnostics), T-F04-103, T-F04-104.

#### T-F04-106. Заполнение fill_id, batch_id, liquidity_source, fee

AC: AC-5.

Файл [cpp/matching/src/app/matching_loop.cpp:179-186](../../cpp/matching/src/app/matching_loop.cpp).

- `fill_id` = `uuid_v4()` per fill;
- `batch_id` = текущий `batch.batch_id()`;
- `liquidity_source` = `"internal"` для всех fills MVP-симулятора;
- `fee` (поле уже в proto): заполнить нулевыми значениями `Fee{amount=0, currency=instrument.quote, fee_bps=0}` либо реальным расчётом из solver_config (после T-F04-110). Для MVP — нули с явным `set_currency`.

Acceptance: в BatchResult каждый FlowFill имеет непустые `fill_id`, `batch_id`, `liquidity_source`; `fee` присутствует.

Зависимости: T-F04-101.

### Фаза 2 — реальный solver за интерфейсом

#### T-F04-110. Извлечь solver в отдельный класс реализации

AC: подготовка к AC-3, AC-4.

Сейчас «псевдо-solver» сидит прямо в [matching_loop.cpp:129-235](../../cpp/matching/src/app/matching_loop.cpp). Извлечь в реализацию интерфейса из [solver.hpp](../../cpp/matching/src/domain/solver.hpp):

- Новый файл `cpp/matching/src/domain/mvp_solver.{hpp,cpp}` — класс `MvpMidpointSolver: IContinuousClearingSolver`. Перенести логику midpoint, distribution, fills assembly.
- Передавать solver через `std::unique_ptr<IContinuousClearingSolver>` в `MatchingLoop` (DI через конструктор).
- `run_one_batch` сводится к: собрать snapshot → `solver_->Solve(snapshot, ref_prices, config)` → publish.

CMakeLists: добавить `src/domain/mvp_solver.cpp` в `add_executable`.

Acceptance: поведение идентично текущему (т.е. AC-2/AC-6 не ломаются); код в `matching_loop.cpp` сокращается до оркестрации.

Зависимости: T-F04-101 (расширенная proto-схема), T-F04-102..T-F04-106 — должны быть мигрированы в `MvpMidpointSolver` без потерь.

#### T-F04-111. Расширить интерфейс Solve

AC: подготовка к AC-3, AC-4.

В [solver.hpp](../../cpp/matching/src/domain/solver.hpp) расширить сигнатуру:

```cpp
struct SolverConfig {
  uint32_t batch_interval_ms;
  uint32_t max_iterations;
  double tolerance;
  double epsilon_liquidity;
  uint32_t config_version;
};

struct ReferencePrices {
  // key: asset (e.g. "BTC"), val: mid/bid/ask
  std::unordered_map<std::string, std::tuple<Decimal, Decimal, Decimal>> by_asset;
};

virtual fob::matching::v1::BatchResult Solve(
    const std::vector<fob::orders::v1::FlowOrder>& active_orders,
    const ReferencePrices& ref_prices,
    const SolverConfig& cfg) = 0;
```

`MvpMidpointSolver` пока игнорирует `ref_prices` (внутри использует midpoint of order's own range), но принимает их в сигнатуре — это позволит позже подложить настоящий solver без рефакторинга.

Acceptance: компиляция зелёная, бенчмарк не деградирует.

Зависимости: T-F04-110.

#### T-F04-112. Реальный equilibrium solver (D(p)=0)

AC: AC-4.

Новый файл `cpp/matching/src/domain/equilibrium_solver.{hpp,cpp}` — реализация `IContinuousClearingSolver`, решающая задачу:

1. Построение функций спроса/предложения `D_i(w_i)` по каждой FlowOrder из CSLO-параметров (`p_low`, `p_high`, `q_rate`, `Q_max`).
2. Решение `D(p) = 0` по вектору цен с ограничениями `x_i ∈ [0, q_i]`.
3. Вычисление `clear_prices[asset]` и `executed_rates[asset]` — единые на инструмент.
4. Распределение исполнений по orders.

Подход для MVP: bisection/Newton по цене per-symbol (separable), max_iterations и tolerance из `SolverConfig`. Для портфельных заявок (F-09) — за пределами этой фичи.

Использовать как DI-альтернатива через env `SOLVER_KIND=equilibrium|mvp_midpoint`.

Acceptance: проходит unit-тесты U1–U10 (см. [test plan](../10-testing/features/F-04-test-plan.md)); инвариант `min(p_L) ≤ clear_price ≤ max(p_H)` ∀ символ.

Зависимости: T-F04-110, T-F04-111.

### Фаза 3 — PostgreSQL solver_config

#### T-F04-120. Добавить libpqxx + миграция

AC: AC-1.

- `cpp/common/CMakeLists.txt`: добавить `find_package(libpqxx REQUIRED)` и опционально слой `cex::common::Pg` в `cpp/common/include/cex/common/pg.hpp` (тонкая обёртка над `pqxx::connection`, конфиг через env).
- `infra/postgres/migrations/0001_solver_config.sql` — создать таблицу из [docs/07-data/oltp-schema.md](../07-data/oltp-schema.md):

  ```sql
  CREATE TABLE solver_config (
    id           SERIAL PRIMARY KEY,
    version      INT NOT NULL UNIQUE,
    batch_interval_ms  INT NOT NULL,
    max_iterations     INT NOT NULL,
    tolerance          DOUBLE PRECISION NOT NULL,
    epsilon_liquidity  DOUBLE PRECISION NOT NULL,
    is_active    BOOLEAN NOT NULL DEFAULT false,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now()
  );
  CREATE UNIQUE INDEX solver_config_active_unique ON solver_config (is_active) WHERE is_active = true;
  ```

- `infra/docker-compose.dev.yml`: добавить service `postgres` (image `postgres:16-alpine`), volume для миграции, env `POSTGRES_DB=fob, POSTGRES_USER=fob, POSTGRES_PASSWORD=fob`.

Acceptance: миграция применяется при старте compose; `solver_config` пустая.

Зависимости: нет.

#### T-F04-121. SolverConfigRepository в matching

AC: AC-1.

Новый файл `cpp/matching/src/infra/solver_config_repo.{hpp,cpp}`:

```cpp
class SolverConfigRepository {
 public:
  SolverConfigRepository(const std::string& pg_dsn);
  domain::SolverConfig LoadActive();
  // Поллит каждые N секунд; если version изменился — атомарно swap'нуть кэш.
  void StartHotReload(std::function<void(const domain::SolverConfig&)> on_change);
  void Stop();
 private:
  cex::common::Pg pg_;
  std::atomic<uint32_t> current_version_{0};
  std::thread reload_thread_;
  std::atomic<bool> running_{false};
};
```

Запросы: `SELECT version, batch_interval_ms, max_iterations, tolerance, epsilon_liquidity FROM solver_config WHERE is_active = true`.

В `main.cpp` — поднимать `SolverConfigRepository`, регистрировать callback, который обновляет `batch_interval_ms_` и переустанавливает `SolverConfig` в `MatchingLoop`. При отсутствии активной строки — fallback на env (текущее поведение), с warning-логом.

Acceptance: изменение `is_active` в БД применяется не позднее `batch_interval_ms` следующего цикла; рестарт не требуется.

Зависимости: T-F04-120, T-F04-111 (SolverConfig тип).

### Фаза 4 — Market Data integration

#### T-F04-130. Расширить marketdata_service.proto

AC: AC-3.

Файл [contracts/proto/fob/marketdata/v1/marketdata_service.proto](../../contracts/proto/fob/marketdata/v1/marketdata_service.proto):

Добавить RPC `GetReferencePrices` со схемой из [docs/06-api/grpc/marketdata-get-reference-prices.md](../06-api/grpc/marketdata-get-reference-prices.md):

```proto
message GetReferencePricesRequest {
  fob.common.v1.EventMeta meta = 1;
  repeated string assets = 2;
  google.protobuf.Timestamp ts_batch = 3;
}
message ReferencePrice {
  string asset = 1;
  fob.common.v1.Decimal mid = 2;
  fob.common.v1.Decimal bid = 3;
  fob.common.v1.Decimal ask = 4;
  google.protobuf.Timestamp as_of = 5;
}
message GetReferencePricesResponse {
  fob.common.v1.EventMeta meta = 1;
  repeated ReferencePrice prices = 2;
  fob.common.v1.Error error = 3;
}
service MarketDataService {
  rpc GetLastTicker(GetLastTickerRequest) returns (GetLastTickerResponse);
  rpc GetReferencePrices(GetReferencePricesRequest) returns (GetReferencePricesResponse);  // <-- new
}
```

Acceptance: proto-build зелёный; [docs/06-api/grpc/marketdata-get-reference-prices.md](../06-api/grpc/marketdata-get-reference-prices.md) статус draft → draft (proto-defined).

Зависимости: нет.

#### T-F04-131. Реализовать GetReferencePrices в market-data

AC: AC-3.

Файлы:

- [cpp/market_data/src/transport/grpc_market_data_service.{hpp,cpp}](../../cpp/market_data/src/transport/) — добавить override `GetReferencePrices`. Источник — тот же `last_ticker` кэш, который уже наполняется из `marketdata.raw`. Для каждого запрошенного `asset` собрать все тикеры пар `{asset}/USDT` или вернуть ошибку, если нет.
- Mapping `asset → canonical pair`: пока хардкод `"BTC" → "BTC/USDT"`; вынести в env `REFERENCE_PAIR_BTC=BTC/USDT` для гибкости.

Acceptance: интеграционный тест — заранее опубликовать в `marketdata.raw` тикер для `BTC/USDT`, вызвать `GetReferencePrices(["BTC"])`, получить непустой mid.

Зависимости: T-F04-130.

#### T-F04-132. MarketDataClient в matching

AC: AC-3.

Новый файл `cpp/matching/src/infra/market_data_client.{hpp,cpp}`:

```cpp
class MarketDataClient {
 public:
  MarketDataClient(const std::string& target);  // "market-data:50051"
  domain::ReferencePrices GetReferencePrices(const std::vector<std::string>& assets,
                                              google::protobuf::Timestamp ts_batch);
 private:
  std::unique_ptr<fob::marketdata::v1::MarketDataService::Stub> stub_;
};
```

CMakeLists: подключить `grpc++` (уже линкуется в gateway/order_flow — повторить тот же блок).

Интеграция в `MatchingLoop::run_one_batch`:

1. Собрать `assets` из `active_` (unique по `instrument.base`).
2. `auto ref = md_client_->GetReferencePrices(assets, ts_batch);`
3. Передать `ref` в `solver_->Solve(...)`.

Acceptance: при остановленном market-data — solver деградирует на fallback (использовать midpoint заявок), при работающем — `ref_prices` присутствуют в diagnostics.

Зависимости: T-F04-130, T-F04-131, T-F04-111.

### Фаза 5 — ClickHouse ingestion

#### T-F04-140. ClickHouse-сервис в compose + DDL

AC: AC-8.

- `infra/docker-compose.dev.yml`: добавить `clickhouse-server` (image `clickhouse/clickhouse-server:24`).
- `infra/clickhouse/migrations/0001_fills_batch_results.sql` — таблицы по [docs/07-data/olap-schema.md](../07-data/olap-schema.md):

  ```sql
  CREATE TABLE fills_kafka (
    batch_id String, fill_id String, order_id String, user_id String,
    symbol String, side Enum8('BUY'=1,'SELL'=2),
    executed_qty Decimal128(18), price Decimal128(18),
    executed_notional Decimal128(18), liquidity_source String,
    fee_amount Decimal128(18), fee_currency String,
    ts DateTime64(3)
  ) ENGINE = Kafka SETTINGS
      kafka_broker_list = 'redpanda:9092',
      kafka_topic_list = 'batch.outputs',   -- или 'fills' после T-F04-150
      kafka_group_name = 'ch-fills',
      kafka_format = 'Protobuf',
      kafka_schema = 'fob/matching/v1/batch.proto:BatchResult';

  CREATE TABLE fills (
    batch_id String, fill_id String, order_id String, user_id String,
    symbol LowCardinality(String), side Enum8('BUY'=1,'SELL'=2),
    executed_qty Decimal128(18), price Decimal128(18),
    executed_notional Decimal128(18), liquidity_source LowCardinality(String),
    fee_amount Decimal128(18), fee_currency LowCardinality(String),
    ts DateTime64(3),
    INDEX idx_order_id order_id TYPE bloom_filter GRANULARITY 1
  ) ENGINE = MergeTree
    PARTITION BY toYYYYMMDD(ts)
    ORDER BY (symbol, ts, fill_id);

  CREATE MATERIALIZED VIEW fills_mv TO fills AS
  SELECT * FROM fills_kafka;
  ```

  Аналогично для `batch_results_kafka` / `batch_results` / `batch_results_mv` (BatchResult без fills repeated).

Acceptance: после старта compose — записи из Kafka batch.outputs появляются в `fills`/`batch_results` MergeTree; consumer lag ≤ 5 s.

Зависимости: T-F04-101 (новые поля fill_id, liquidity_source).

#### T-F04-141. Тест ingestion lag и идемпотентности

AC: AC-8.

Скрипт `tests/integration/f04/clickhouse_ingest_test.sh`:

1. Запустить docker-compose.
2. Опубликовать N=100 синтетических BatchResult.
3. Через 10 секунд — `SELECT count() FROM fills` должен дать N (или больше, если есть повторы).
4. Опубликовать те же события ещё раз → count в CH удваивается (Kafka engine — at-least-once); добавить `FINAL` или `argMax` для дедупликации в read-time, либо использовать `ReplacingMergeTree` с `version`.

Acceptance: либо `ReplacingMergeTree(version) ORDER BY fill_id`, либо документированный paradigm «sum/group by fill_id с dedup в queries» — выбрано и зафиксировано в [docs/07-data/olap-schema.md](../07-data/olap-schema.md).

Зависимости: T-F04-140.

### Фаза 6 — Топик split

#### T-F04-150. Раздельные топики batch.outputs + fills

AC: AC-7. Закрывает Conflict Note C-1.

Изменения:

1. [infra/kafka/create_topics.sh](../../infra/kafka/create_topics.sh) — добавить топик `fills` с партиционированием по `order_id`.
2. [cpp/matching/src/app/matching_loop.cpp](../../cpp/matching/src/app/matching_loop.cpp) — после публикации `batch.outputs` (BatchResult без `fills` repeated) дополнительно публиковать N сообщений в `fills` (по одному per FlowFill, key = `order_id`). На время миграции — публиковать в оба места (BatchResult.fills для legacy consumers + отдельный topic для новых).
3. [cpp/ledger/src/infra/kafka_consumers.cpp:60](../../cpp/ledger/src/infra/kafka_consumers.cpp) — переключить на `fills` topic (опционально через env флаг `LEDGER_FILLS_SOURCE=batch_outputs|fills_topic`).
4. [docs/06-api/messaging/fills.md](../06-api/messaging/) — новый contract doc.
5. Conflict Note C-1 в [topics.md](../06-api/messaging/topics.md) → перевести в `resolved` с описанием миграционного периода.
6. Coverage matrix: F-04 строка → `✅ batch.outputs/fills` без planned suffix.

Acceptance:

- интеграционный тест: один BatchResult → один `batch.outputs` event + N `fills` events;
- ledger корректно применяет fills из нового топика;
- backward-compat: старые consumers с `batch.outputs.fills` repeated продолжают работать в течение миграционного периода.

Зависимости: T-F04-101 (fill_id, liquidity_source), T-F04-140 (CH ingestion — переключить источник на `fills`).

### Фаза 7 — WebSocket streaming

#### T-F04-160. WS-эндпоинт в gateway

AC: AC-9.

Файлы:

- [cpp/gateway/src/transport/](../../cpp/gateway/src/transport/) — добавить `ws_gateway.{hpp,cpp}`. Использовать `boost::beast` (уже доступен через `vcpkg`/`Conan` — проверить или добавить).
- Endpoint `GET /v1/stream?topics=batch.outputs,fills` — Upgrade на WebSocket.
- Внутри: KafkaConsumer на запрошенные топики (`group_id = ws-{conn_id}`), JSON-сериализация proto → client.
- Heartbeat ping каждые 30 s, graceful close на разрыв.

Acceptance:

- `wscat -c ws://localhost:8088/v1/stream?topics=batch.outputs` получает события после `RunBatch`;
- 100 параллельных соединений без падений.

Зависимости: T-F04-150 (separate fills topic — чтобы клиенты подписывались гранулярно).

### Фаза 8 — Тесты

#### T-F04-170. Unit-тесты U1–U10

AC: AC-13 + проверки solver инвариантов.

Создать `cpp/matching/tests/` (директории сейчас нет):

- `tests/CMakeLists.txt` (GoogleTest или Catch2);
- `tests/solver_test.cpp` — фикстуры U1–U10 из [test plan](../10-testing/features/F-04-test-plan.md);
- `cpp/matching/CMakeLists.txt` — `add_subdirectory(tests)` под условием `BUILD_TESTING`.

Тесты вызывают `EquilibriumSolver::Solve(...)` напрямую, без Kafka/PG.

Acceptance: все 10 тестов проходят на CI; невыполнение хотя бы одного — fail.

Зависимости: T-F04-112.

#### T-F04-171. Integration-тест полного цикла

AC: AC-8 + полный pipeline.

Файлы: `tests/integration/f04/full_cycle_test.{cpp|sh}`. Сценарий:

1. docker-compose up (postgres, redpanda, clickhouse, market-data, matching, ledger, observability).
2. Прелейд: insert в `solver_config` (is_active=true).
3. Опубликовать N=10 FlowOrder через order-flow (gRPC).
4. Подождать batch_interval_ms × 2.
5. Проверить:
   - в Kafka `batch.outputs` 1+ событие;
   - в Kafka `fills` N+ событий;
   - в CH `fills` тоже N строк;
   - в CH `batch_results` 1+ строка;
   - в ledger балансы изменены ожидаемо.

Acceptance: тест проходит на CI nightly.

Зависимости: T-F04-140, T-F04-150, T-F04-120.

#### T-F04-172. SLA-тест nightly

AC: AC-10, AC-11.

Скрипт `tests/performance/f04/sla_test.py`:

1. Прелейд N ∈ {50, 200, 500, 1000} FlowOrder.
2. Прогнать ≥ 1000 батчей.
3. Замерить p50 / p95 / p99 `solve_time_ms` из `batch.outputs.diagnostics.solve_time_ms`.
4. Сравнить с порогами таблицы [NFR-EXEC-002](../02-system/non-functional-requirements.md#nfr-exec-002-solver-latency).

Acceptance: p95 ≤ цель, p99 ≤ жёсткий предел для всех N.

Зависимости: T-F04-103, T-F04-112.

## Сводная зависимость фаз

```text
Фаза 1 (proto + quick wins)   T-F04-101..106
  ├──> Фаза 2 (real solver)   T-F04-110..112
  │      └──> T-F04-170 (U1–U10)
  ├──> Фаза 3 (Postgres)      T-F04-120..121
  │      └──> T-F04-171
  ├──> Фаза 4 (Market Data)   T-F04-130..132
  │      └──> T-F04-171
  ├──> Фаза 5 (ClickHouse)    T-F04-140..141
  │      └──> T-F04-150 (split topics) ──> T-F04-160 (WS)
  └──> T-F04-105 (alerts)
```

## Маппинг AC → задача

| AC | Closing tasks |
| --- | --- |
| AC-1 | T-F04-120, T-F04-121 |
| AC-2 | (уже ✅; ничего) |
| AC-3 | T-F04-130, T-F04-131, T-F04-132 |
| AC-4 | T-F04-110, T-F04-111, T-F04-112 |
| AC-5 | T-F04-101, T-F04-106 |
| AC-6 | (уже ✅; ничего) |
| AC-7 | T-F04-101, T-F04-150 |
| AC-8 | T-F04-101, T-F04-140, T-F04-141 |
| AC-9 | T-F04-160 |
| AC-10 | T-F04-103, T-F04-172 |
| AC-11 | T-F04-103, T-F04-172 |
| AC-12 | T-F04-101, T-F04-104 |
| AC-13 | T-F04-102, T-F04-170 |
| AC-14 | T-F04-101, T-F04-104, T-F04-105 |
| AC-15 | T-F04-101, T-F04-103, T-F04-105 |

## Оценка трудозатрат (порядок величин)

| Фаза | Задачи | Усилия |
| --- | --- | --- |
| 1: proto + quick wins | T-F04-101..106 | 1–2 дня |
| 2: solver behind interface + equilibrium | T-F04-110..112 | 5–10 дней (зависит от LP/QP-формулировки) |
| 3: PostgreSQL solver_config | T-F04-120..121 | 2–3 дня |
| 4: Market Data gRPC | T-F04-130..132 | 2–3 дня |
| 5: ClickHouse ingestion | T-F04-140..141 | 2–3 дня |
| 6: Topic split | T-F04-150 | 1–2 дня (+ миграция consumers) |
| 7: WebSocket | T-F04-160 | 2–3 дня |
| 8: Тесты | T-F04-170..172 | 3–5 дней |

Итого: **18–31 рабочий день** для одного разработчика. Параллелизация даёт ~10 дней календарных при двух разработчиках.

## Next

После завершения T-F04-001..008 и T-F04-101..172 — обновить статус F-04 в [`coverage-matrix.md`](../traceability/coverage-matrix.md) до `complete` и пересмотреть SLA NFR-EXEC-002.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028 (feature baseline)
- IN-003-FR-014 (functional requirements F4-1..F4-6)
- IN-003-FR-015 (NFR)
- IN-003-FR-020 (алгоритм)
- IN-003-FR-021 (методы)
- IN-003-FR-022..025 (testing)
- IN-003-FR-026 (DoD)
