---
id: DOC-TEST-F-04
phase: 10-testing
status: draft
owner: core-team
source:
  - IN-003 §5 «Тестирование»
related:
  - docs/02-system/features/F-04-batch-clearing/
  - docs/implementation-plan/F-04-batch-clearing.tasks.md
  - docs/02-system/non-functional-requirements.md
---

# F-04 Batch Clearing — план тестирования

Полный спецификационный источник: [`incoming-docs/2026-05-13-F-04-Batch-Clearing-v1.md`](../../../incoming-docs/2026-05-13-F-04-Batch-Clearing-v1.md) §5.

## 1. Юнит-тесты солвера

Тестируется чистая функция `solveBatch(batch_id, flow_orders, reference_prices) → BatchResult, FillEvent[]` без PostgreSQL, Kafka и UI.

Три направления:

1. **Корректность решения** на простых сценариях (U1–U3, U6–U7).
2. **Инвариант диапазона цен** (U4–U5, U10): `min(p_L,i) ≤ clear_price ≤ max(p_H,i)`.
3. **Поведение `residual_norm` и условий остановки** (U8–U9): `stopped_by_tolerance` vs `stopped_by_max_iterations`.

### Таблица U1. Симметричные buy/sell, один актив

| Test | FlowOrder 1 | FlowOrder 2 | RefPrice | Ожидание |
| --- | --- | --- | --- | --- |
| U1 | buy, p_L=99, p_H=101, q_rate=1, Q_max=10 | sell, p_L=99, p_H=101, q_rate=1, Q_max=10 | mid=100, bid=99, ask=101 | clear=100; rate1=1; rate2=−1; total buy = total sell |
| U2 | buy, q_rate=2, Q_max=10 | sell, q_rate=1, Q_max=10 | mid=100 | clear≈100; rate1=1; rate2=−1 (ограничен меньшим q_rate) |
| U3 | buy, Q_max=2 | sell, Q_max=10 | mid=100 | clear=100; qty1=2 (выбил Q_max1), qty2=2; статус1=FILLED, статус2=ACTIVE |

### Таблица U2. Несимметричные диапазоны цен

| Test | FlowOrder 1 | FlowOrder 2 | RefPrice | Ожидание |
| --- | --- | --- | --- | --- |
| U4 | buy, p_L=99, p_H=100 | sell, p_L=100, p_H=101 | mid=100 | clear=100; rate1=1; rate2=−1 |
| U5 | buy, p_L=99, p_H=100 | sell, p_L=101, p_H=102 | mid=100 | clear ∈ допустимом интервале, но `executed_rates`≈0 (нет реального пересечения) |

### Таблица U3. Портфельный 2-активный

| Test | A | B | RefPrice (BTC, ETH) | Ожидание |
| --- | --- | --- | --- | --- |
| U6 | buy: +1 BTC, −15 ETH | sell: −1 BTC, +15 ETH | BTC=60000, ETH=4000 | `asset_legs` зеркальны: A {+X BTC, −15X ETH}; B {−X BTC, +15X ETH}; суммы в системе = 0 |
| U7 | A как в U6; B = два FlowOrder (BTC и ETH отдельно), эквивалентные портфелю | как в U6 | те же | Solver матчит портфель A с набором ног B; сумма legs по всем fills = U6 |

### Таблица U4. Сходимость / несходимость

| Test | Описание | solver_config | Ожидание |
| --- | --- | --- | --- |
| U8 | Идеально-решаемый симметричный кейс | tolerance=1e-6, max_iterations=1000 | `residual_norm ≤ 1e-6`; iterations ≪ max; `stopped_by_tolerance=true` |
| U9 | Сконструированный жёсткий кейс | tolerance=1e-12, max_iterations=10 | iterations == 10; `residual_norm > tolerance`; `stopped_by_max_iterations=true` |

### Таблица U5. Инвариант по диапазону цен

| Test | Набор FlowOrder | Ref mid | Ожидание |
| --- | --- | --- | --- |
| U10 | Сгенерированный набор `p_L`/`p_H` | mid=100 | ∀ инструмент: `min(p_L) ≤ clear_price ≤ max(p_H)` |

## 2. Интеграционные тесты (контейнеры + БД + Kafka)

Тестируется связка `matching-fob-core` + `ledger` (PostgreSQL) + `market-data` + Kafka + ClickHouse.

### 2.1. Полный цикл данных

1. В тестовом PostgreSQL — несколько записей в `flow_orders` со `status='active'`.
2. Тестовый Market Data Service возвращает фиксированный mid/bid/ask.
3. Запустить Matching Backend с одноразовым Scheduler-тиком, вызывающим `RunBatch(test_batch_id, ts_batch)`.

Проверки:

- В Kafka `batch.outputs` — одно сообщение с `batch_id == test_batch_id`, корректные `clear_prices`, `executed_rates`, `residual_norm`, `solve_time_ms`.
- В Kafka (текущий MVP — внутри того же `batch.outputs`; planned split — `fills`) — `FillEvent` на каждую исполнившуюся заявку (см. Conflict Note C-1).
- ClickHouse `fills`: появились строки для каждого FillEvent.
- ClickHouse `batch_results`: запись с `batch_id == test_batch_id`.

Инварианты:

- Суммарный buy по активу = суммарному sell.
- Суммы по `asset_legs` бьются между участниками.
- `liquidity_source` имеет ожидаемое значение (например, `internal` для внутреннего матчинга).

### 2.2. SLA-тест по solve time

Цель: убедиться, что при типовых размерах батча solver укладывается в SLA из [NFR-EXEC-002 таблицы](../../02-system/non-functional-requirements.md#nfr-exec-002-solver-latency).

Сценарий:

1. Сгенерировать 1000 активных FlowOrder с реалистичными параметрами.
2. Прогнать ≥ 1000 батчей подряд.
3. Для каждого батча зафиксировать `solve_time_ms` из `BatchResult` + фактическое время `RunBatch` на стороне Matching Backend.

Критерии прохождения:

- p95 ≤ цель из таблицы NFR-EXEC-002 для соответствующего диапазона N.
- p99 ≤ жёсткий предел.
- Доля батчей с `residual_norm > tolerance` ≤ 1%.

### 2.3. Repeatability / Replay

При двукратном прогоне с теми же входными `flow_orders` и `reference_prices` и тем же `solver_config.version` результаты `BatchResult` и `FillEvent[]` идентичны побайтово (за исключением `solve_time_ms`).

## 3. Ручные тесты

### 3.1. Функциональный сценарий

1. Завести 2–3 demo-аккаунта (Auth & Identity).
2. Через Web UI / API Gateway создать понятные FlowOrder:
   - Клиент A: buy BTCUSDT, узкий диапазон вокруг mid, небольшой объём.
   - Клиент B: sell BTCUSDT в том же диапазоне и объёме.
   - Клиент C: заявка, которая не должна исполниться (слишком смещённый диапазон).
3. Дождаться батча (или вручную триггернуть RunBatch в тестовом окружении).
4. Проверить через UI и базу:
   - У A и B появились FillEvent с зеркальными объёмами;
   - `clear_price` по BTCUSDT близок к mid и попадает в объединённый интервал `p_L`/`p_H`;
   - Заявка C осталась без исполнения (`exec_qty=0`, `status=active`).

Повторить с портфельной заявкой (BTC+ETH) — проверить пропорции по обеим ногам.

### 3.2. Диагностический сценарий

1. Открыть экран «Диагностика клиринга».
2. После выполнения батча убедиться, что отображаются: `batch_id`, время, `clear_prices`, `executed_rates`, `residual_norm`, `solve_time_ms`, флаги/поля из `solver_diagnostics`.
3. Drill-down: при выборе батча видны его FillEvent с фильтрацией по клиентам/инструментам и параметры участвовавших FlowOrder.

Полезно воспроизвести U1/U2 руками и сверить экран диагностики с ожидаемым решением.

### 3.3. Негативный сценарий (нестабильное решение)

1. Подготовить набор FlowOrder с почти конфликтующими диапазонами и сильно разными `q_rate/Q_max`.
2. Временно установить жёсткий `solver_config`: `tolerance=1e-12`, `max_iterations=10`.
3. Запустить батч:
   - `residual_norm` заметно выше обычного.
   - В `solver_diagnostics` — `stopped_by_max_iterations=true`.
   - `solve_time_ms` выше нормы, но в допустимых пределах.
4. Проверить реакцию системы:
   - На экране диагностики `residual_norm` подсвечен.
   - При превышении порога (`residual_norm > 0.01`) Observability формирует alert.
   - При критическом значении — Risk Manager может выставить kill-switch или сформировать `risk_event`.

## 4. Подключение к task plan и DoD

Каждая позиция чек-листа [DoD F-04](../../implementation-plan/F-04-batch-clearing.tasks.md#definition-of-done-in-003) ссылается на конкретные тесты из этого плана.

## Source Fragments

- IN-003-FR-022 (юнит-тесты концепции)
- IN-003-FR-023 (таблицы U1–U10)
- IN-003-FR-024 (SLA таблица по размеру батча)
- IN-003-FR-025 (ручные тесты)
