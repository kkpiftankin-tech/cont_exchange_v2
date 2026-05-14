# F-04. Batch Clearing Cycle — детальная спецификация v1 (IN-003)

> **Архивный источник**, передан пользователем 2026-05-13. Восстановлен в читаемую кириллицу из mojibake (latin-1 ↔ utf-8). Структура и нумерация разделов сохранены 1:1.
>
> Этот файл — **immutable архив** для F-04. Все нормализованные артефакты лежат в:
>
> - meta: [IN-003.meta.md](IN-003.meta.md);
> - fragment map: [IN-003.fragment-map.md](IN-003.fragment-map.md);
> - целевые документы перечислены в [`docs/traceability/source-to-artifact-map.md`](../docs/traceability/source-to-artifact-map.md).

---

# 1. Общая информация

## 1.1. Понятия и определения

### Базовые сущности batch-клиринга

- **Batch (батч).** Один шаг клиринга. Система одновременно рассматривает все активные FlowOrder за интервал времени и решает, по каким ценам и в каких объёмах их исполнить.
- **BatchResult.** Объект-сводка по батчу: `batch_id`, `clear_prices` (вектор клиринговых цен по инструментам), `executed_rates` (эффективные скорости исполнения, tokens/sec), `residual_norm` (численная мера «нерасчищенного» спроса/предложения), `solve_time_ms`, `solver_diagnostics` (итерации, флаги, доп. поля).
- **FlowOrder.** Потоковая заявка клиента: `p_low`, `p_high`, `q_rate`, `q_max`, `filled_cum`, `status`.
- **FillEvent (fills).** Запись об отдельном исполнении: `fill_id`, `batch_id`, `order_id`, `asset_legs` (для пар/портфелей), `exec_qty`, `exec_price`, `liquidity_source` ∈ {internal, cex_hedge, dex_hedge, epsilon_mm}, `fees`, `timestamp`.

### Поля и величины solver

- **clear_prices.** Вектор клиринговых цен по инструментам, при которых решается система спрос/предложение D(p) ≈ 0.
- **executed_rates.** Для каждого инструмента — итоговая скорость исполнения (агрегированное v_t в этом батче).
- **residual_norm.** Численная норма остатка `residual` после решения задачи клиринга. `residual` описывает несоответствие между спросом и предложением при найденных ценах; `residual_norm` — агрегирующая метрика (например, евклидова норма). Используется для контроля качества решения и триггера fallback/ошибки.
- **solve_time_ms.** Время работы солвера для батча в миллисекундах. Используется для SLA (median ≤ 500 ms, p95 ≤ 1000 ms).
- **solver_config.** Конфигурация солвера, хранимая в PostgreSQL:
  - `batch_interval_ms` — интервал между батчами;
  - `max_iterations` — максимум итераций алгоритма;
  - `epsilon_liquidity` — параметр epsilon-MM (резервная ликвидность);
  - `tolerance` — порог для `residual_norm`;
  - `fee_model` — описание комиссий (maker/taker, speed-dependent);
  - `is_active` — флаг активной конфигурации.
- **batch_interval_ms.** Период batch-клиринга в `solver_config`. Например, 1000 ms = новый батч каждую секунду.

### Транспорт и хранилища

- **Kafka `batch.outputs`.** Топик, в который Matching Backend публикует результаты batch-клиринга (BatchResult и/или ссылки на FillEvent). Слушатели: Ledger, Risk, Market Data, Observability, Backtest.
- **fills (таблица ClickHouse).** Таблица аналитического хранилища с FillEvent. Используется для отчётности, IS/VWAP, backtest, мониторинга.
- **flow_orders (таблица PostgreSQL).** OLTP-таблица активных/исторических FlowOrder. Matching Backend читает её на старте батча.
- **batch_results (таблица ClickHouse).** Одна запись на батч с полями BatchResult (cены, скорости, residual_norm, solve_time_ms, диагностика).

### Источники ликвидности

- **internal.** Исполнение за счёт встречных FlowOrder других клиентов внутри биржи (внутренний клиринг).
- **cex_hedge / dex_hedge.** Исполнение, физически хеджируемое на внешних CEX/DEX через Execution Hedge (F-12).
- **epsilon_MM.** Биржа выступает market-maker'ом последней инстанции на малый объём ε, чтобы сгладить несоответствия и обеспечить ликвидность, когда внутренних ордеров недостаточно. Объём задаётся `epsilon_liquidity` и политикой MM.

### Время и SLA

- **SLA.** Целевые метрики времени решения батча: median и p95 `solve_time_ms` не должны превышать заданных значений (500 ms и 1000 ms для MVP). Нужны для предсказуемой задержки.
- **median / p95 solve time.** Статистики распределения `solve_time_ms` для множества батчей. Используются для мониторинга и триггера alert'ов/kill-switch.

### Прочие термины

- **Downstream-сервисы.** Сервисы-потребители результатов batch-клиринга: Collateral Ledger (позиции/PnL), Risk Manager (post-trade VaR/margin), Market Data Service (публикация клиринговых цен), Web UI (отображение).
- **Deterministic execution (детерминированное исполнение).** Свойство F-04: при одинаковых входных FlowOrder и market data солвер всегда выдаёт один и тот же BatchResult/FillEvent. Критично для replay, аудита и справедливости.
- **Честное исполнение.** Уважает лимиты `p_low`, `p_high`, `q_rate`, `q_max` каждого FlowOrder; применяет одинаковые правила ко всем участникам батча; не даёт скрытых преимуществ отдельным клиентам.

## 1.2. Краткое описание

Фича отвечает за периодический запуск солвера, расчёт клиринговых цен и скоростей по всем активным FlowOrder, генерацию BatchResult и FillEvent, публикацию результатов в downstream-сервисы (Ledger, Risk, Market Data, UI).

## 1.3. Область применения

- Ядро биржи FOB (Matching Backend).
- Все инструменты, поддерживаемые FlowOrder (single-asset, pair, multi-leg).

## 1.4. Заинтересованные стороны

- Трейдеры/клиенты (получают исполнение по своим FlowOrder).
- Операторы/risk-офицеры (контролируют качество клиринга, SLA, риск).
- Команды Matching, Risk, Ledger, Market Data, Observability.

---

# 2. Бизнес-часть (BRD)

## 2.1. Назначение и цели

Функция предназначена для:

- периодического «сведения» активных потоковых заявок клиентов в общем батче;
- определения равновесных цен и скоростей исполнения для данного интервала времени;
- записи результатов клиринга для дальнейшего учёта и анализа.

Цели:

- честное и детерминированное исполнение FlowOrder в батчах с учётом их лимитов `p_low`, `p_high`, `q_rate`, `q_max`;
- SLA: median solve_time ≤ 500 ms, p95 ≤ 1000 ms.

## 2.2. Предусловия

- Пользователи авторизованы (F-01), имеют счета и балансы.
- В системе есть активные FlowOrder со статусом `active`, прошедшие pre-trade проверку (F-02, F-07).
- Доступны reference price и базовый market data (mid, bid/ask) от Market Data Service.
- Конфигурация солвера (`solver_config`) задана и активна.

## 2.3. Основной успешный сценарий

### 2.3.1. Бизнес-уровень

Участники: Пользователь (клиент/трейдер), Система биржи (FOB).

Сценарий:

1. Пользователь заранее создаёт одну или несколько FlowOrder.
2. Система каждые `batch_interval_ms` собирает все активные FlowOrder.
3. Рассчитывает клиринговые цены и скорости исполнения для всех инструментов.
4. Определяет объём исполнения по каждой FlowOrder в этом батче.
5. Фиксирует BatchResult и FillEvent, обновляет позиции/балансы.
6. Пользователь в UI видит fills, позиции и клиринговые цены.

### Альтернативные сценарии (§2.4)

- **Нет активных заявок.** Батч создаётся с пустыми `executed_rates`, без FillEvent. Лог/метрика пустого батча в Observability.
- **Солвер не сошёлся.** `residual_norm > tolerance` или достигнут `max_iterations`. BatchResult создаётся с флагом ошибки; возможен fallback (epsilon-MM, остановка торгов).
- **Нарушен SLA `solve_time_ms`.** BatchResult с признаком нарушения SLA; risk/observability alert; при необходимости kill-switch (F-16).

## 2.5. UX / UI

- Экран «Торговля / Активные заявки»: FlowOrders с `filled_cum`, лимиты `p_low`, `p_high`, `q`, `Q_max`, фильтры, действия.
- Экран «Исполнения / История клиринга»: список FillEvent с ценой, объёмом, batch_id, источником ликвидности.
- Экран «Диагностика клиринга»: для операторов — `clear_prices`, `executed_rates`, `residual_norm`, `solve_time_ms` по батчам.

## 2.6. Критерии успеха

- Для каждого батча рассчитан BatchResult с валидными `clear_prices`, `executed_rates`, `residual_norm`, `solve_time_ms`.
- FillEvent корректно отражают объёмы и цены исполнения по всем FlowOrder.
- SLA по solve time соблюдается на production-нагрузке (по метрикам Observability).

---

# 3. Требования

## 3.1. Функциональные требования

- **F4-1.** Система должна запускать цикл batch-клиринга с интервалом `batch_interval_ms` из `solver_config`.
- **F4-2.** На каждый батч система должна собирать список активных FlowOrder и корректные reference price.
- **F4-3.** Система должна рассчитывать клиринговые цены и скорости исполнения для всех инструментов в батче (solver).
- **F4-4.** Для каждой FlowOrder система должна определять объём исполнения и генерировать FillEvent.
- **F4-5.** Система должна сохранять BatchResult и FillEvent в ClickHouse и публиковать их в Kafka (`batch.outputs`, `fills`).
- **F4-6.** Система должна транслировать клиринговые цены и новые fills в UI по WebSocket.

## 3.2. Нефункциональные требования

- Median `solve_time_ms ≤ 500 ms`, p95 ≤ 1000 ms при целевом N активных заявок.
- `residual_norm` должна быть ниже заданного порога `tolerance`, иначе батч помечается как деградировавший.
- Система должна быть детерминированной: при одинаковых входных данных — одинаковый BatchResult.
- При отказах (ошибка солвера, таймаут) — понятная деградация (логирование, alert'ы, опциональный kill-switch).

---

# 4. Техническая архитектура (TRD)

## 4.1. Состав компонентов

- PostgreSQL (таблица `flow_orders` — набор активных заявок; `solver_config`).
- Matching Backend / solver (C++/Go) — расчёт клиринга, генерация BatchResult и FillEvent.
- Market Data Service — reference price и bid/ask для расчёта и диагностики.
- Kafka — топики `batch.outputs` (BatchResult) и `fills` (FillEvent).
- Collateral Ledger, Risk Manager — потребители `fills`/`batch.outputs`.
- ClickHouse — хранение `batch_results` и `fills`.
- Web UI — подписка по WebSocket на клиринговые цены и исполнения.

## 4.2. Диаграмма последовательности (техническая)

Участники: Scheduler → Matching Backend (solver) → PostgreSQL → Market Data Service → Kafka → Collateral Ledger / Risk Manager / Market Data / Web UI.

Описание одного цикла:

1. Scheduler в Matching Backend запускает батч. Scheduler — это внутренний «будильник» Matching Backend, который по расписанию запускает очередной батч клиринга.
2. Matching Backend читает активные FlowOrder из PostgreSQL (`flow_orders WHERE status='active'`).
3. Matching Backend запрашивает reference prices у Market Data Service.
4. Солвер считает клиринговые цены/скорости, затем объёмы исполнения.
5. Matching Backend пишет BatchResult и FillEvent в Kafka (`batch.outputs` и `fills`), а также в ClickHouse (через ingestion).
6. Ledger и Risk читают `fills`/`batch.outputs`, обновляют свои состояния; Web UI через Market Data Service/WS получает обновлённые цены клиринга и fills.

### Расширенный поток (User → UI → Backend → Stores)

1. Пользователь на Web UI вводит параметры заявки; UI отправляет их в API Gateway по HTTP.
2. API Gateway проверяет аутентификацию, лимиты по запросам и маршрутизирует запрос в Provider-Client backend.
3. Provider-Client преобразует дискретный ввод (символ, объём, окно, тип) в нормализованный FlowOrder с параметрами `p_L`, `p_H`, `q`, `Q_max` и сохраняет его в PostgreSQL как активный.
4. После успешной записи пользователь через UI получает подтверждение и видит свою активную FlowOrder.
5. Периодически, с шагом `batch_interval_ms` из `solver_config`, Matching Backend запускает цикл клиринга.
6. Matching Backend читает из PostgreSQL все FlowOrder со статусом `active` на текущий батч.
7. Для расчёта клиринга Matching Backend запрашивает у Market Data Service текущие reference-цены и bid/ask по нужным инструментам.
8. Солвер в Matching Backend на основе FlowOrder и цен решает задачу клиринга: вычисляет клиринговые цены (`clear_prices`) и скорости исполнения (`executed_rates`).
9. По результатам решения он определяет, какой объём исполнения приходится на каждую FlowOrder в этом батче.
10. Matching Backend формирует BatchResult (агрегированная сводка батча) и набор FillEvent (конкретные исполнения по ордерам).
11. Эти BatchResult и FillEvent публикуются в Kafka в топики `batch.outputs` и `fills`, чтобы другие сервисы могли их потреблять.
12. Collateral Ledger читает FillEvent из Kafka, обновляет позиции, PnL и маржинальные состояния клиентов и системы.
13. Risk Manager читает BatchResult и FillEvent, выполняет post-trade риск-контроль (VaR/CVaR, margin shortfall, алерты).
14. Когда пользователь запрашивает свои позиции/исполнения, Provider-Client через API получает данные из Ledger и (при необходимости) из ClickHouse/Market Data, а затем отдаёт их через API Gateway в Web UI.
15. Web UI показывает пользователю актуальные fills, позиции и клиринговые цены.

## 4.3. Техническая интеграция

### 4.3.1. Интеграции

- Matching Backend → PostgreSQL: SQL-запросы к `flow_orders`, `solver_config`.
- Matching Backend → Market Data Service: HTTP/gRPC запрос за reference prices или подписка на `marketdata.raw`.
- Matching Backend → Kafka: публикация `batch.outputs` (BatchResult) и `fills` (FillEvent).
- Kafka → Ledger, Risk, Market Data: потребление событий.

### 4.3.2. Изменяемые объекты конфигурации

- Таблица `solver_config` в PostgreSQL (batch_interval_ms, max_iterations, epsilon_liquidity, tolerance, fee_model).
- Настройки Kafka topics (retention, partitions).

## 4.4. Способ реализации

### 4.4.1. Данные

**Вход:**

- FlowOrder (поля `w_i`, `p_L`, `p_H`, `q_i`, `Q_max`, status=active):
  - `w_i` — портфельные веса по активам (для портфельных заявок; для single-asset подразумевается простое отображение «1 единица в 1 quote»).
  - `p_L`, `p_H` — диапазон цен.
  - `q_i` — максимальная скорость торговли.
  - `Q_max` — общий лимит объёма по заявке.
  - `status=active` — только активные заявки попадают в solver.
- Reference prices (mid, bid/ask) по символам.

**Выход:**

- BatchResult: `batch_id`, `clear_prices`, `executed_rates`, `residual_norm`, `solve_time_ms`, `solver_diagnostics`.
- FillEvent: `fill_id`, `batch_id`, `order_id`, `asset_legs`, `exec_qty`, `exec_price`, `liquidity_source`, `fees`, `timestamp`.

**Связь:**

1. На начало батча есть список активных FlowOrder и reference prices.
2. Solver решает задачу клиринга: находит `clear_prices` и `executed_rates`, минимизируя функцию стоимости и соблюдая ограничения модели.
3. По найденным `clear_prices` и `executed_rates` для каждого FlowOrder вычисляется, сколько он реально «купил/продал» за этот батч, по какой эффективной цене, по каким активам.
4. Это превращается в один BatchResult на батч и набор FillEvent — по одному (или несколько) на каждую заявку, где что-то исполнилось.
5. Дальше Risk/Ledger читает FillEvent и BatchResult и обновляет маржу, позиции и PnL (F-06, F-08); Frontend и Provider-Client используют FillEvent, чтобы показать клиентам, что у них исполнилось; Exchange-provider использует FillEvent по своим FlowOrder, чтобы хеджировать внешнюю позицию.

### 4.4.2. Алгоритм (упрощённо)

1. Собрать активные FlowOrder на время батча.
2. Построить функции спроса/предложения `D_i(w_i)` по каждому ордеру на основе CSLO (F-02).
3. Решить систему `D(p) = 0` по вектору цен/скоростей с ограничениями `x_i ∈ [0, q_i]`.
4. Вычислить `clear_prices` и `executed_rates` для каждого инструмента.
5. Для каждого FlowOrder вычислить `exec_qty`, `exec_price`, `fees`, источники ликвидности, сформировать FillEvent.
6. Посчитать `residual_norm` и `solve_time_ms`, собрать BatchResult.
7. Записать BatchResult и FillEvent в ClickHouse / Kafka.

### 4.4.3. Интерфейсы (обновлённая формулировка)

**Внешние интерфейсы.**

F-04 сама не добавляет новых HTTP-endpoint'ов. Она использует существующие интерфейсы:

- `API Gateway (Provider-Client)` — приём и управление FlowOrder (F-02/F-03), которые потом читает Matching Backend.
- `Web UI / Trading Frontend` — читает результаты батча через REST `GET /batches/{id}` и WebSocket `ws/market` для стриминга цен и fills.

**Внутренние интерфейсы по контейнерам.**

`Matching Backend (FOB Core)`. Внутри живут и Scheduler, и Clearing Solver — это не отдельные сервисы, а части одного контейнера.

Основные методы:

- `RunBatch(batchId: string, tsBatch: timestamp)` — вызывается внутренним Scheduler'ом по таймеру. Логика:

  1. `orders = Collateral & Ledger.loadActiveFlowOrders(tsBatch)` — SQL-чтение из OLTP PostgreSQL: `SELECT * FROM flow_orders WHERE status='active' AND window_start <= tsBatch <= window_end`.
  2. `assets = collectAssetsFrom(orders)`.
  3. `refPrices = Market Data Service.getReferencePrices(assets, tsBatch)`.
  4. `solution = solveBatch(batchId, orders, refPrices)`.
  5. `Collateral & Ledger.saveBatchResult(solution.batchResult)`.
  6. `Collateral & Ledger.saveFills(solution.fillEvents)`.
  7. `Kafka.publish("batch.outputs", solution.batchResult)`.
  8. `Kafka.publish("fills", solution.fillEvents)`.

- `solveBatch(batchId, orders, refPrices) → { batchResult, fillEvents[] }` — строит задачу клиринга, вызывает численный оптимизатор, формирует BatchResult и FillEvent.

DTO: FlowOrder, BatchResult, FillEvent, ReferencePrice.

Scheduler — внутренний таймер Matching Backend, не отдельный контейнер.

`Market Data Service`:

- `getReferencePrices(assets: string[], tsBatch: timestamp) → map<asset, ReferencePrice>`.
- Использует Kafka `marketdata.raw` и/или VenueSnapshot из External Venues Connector; ClickHouse / собственный cache для вычисления mid / bid / ask.
- DTO ReferencePrice: `asset`, `mid`, `best_bid`, `best_ask`, `source`, `timestamp`.

`Collateral & Ledger (OLTP PostgreSQL)`:

- `loadActiveFlowOrders(tsBatch) → FlowOrder[]` — SQL-view/DAO над `flow_orders`.
- `saveBatchResult(batchResult)` — запись агрегата батча в `batch_results` (PostgreSQL или ClickHouse — зависит от реализации).
- `saveFills(fills[])` — запись исполнений в OLTP (для оперативного UI и последующего пересчёта позиций).

Обновление маржи/позиций по этим fills — зона F-06/F-08, там Risk Manager и Settlement & Ledger.

`Message Broker (Apache Kafka)`. Топики для F-04:

- `batch.outputs` — сообщения типа `BatchResult`. Писатель: Matching Backend.
- `fills` — сообщения типа `FillEvent`. Писатель: Matching Backend.

Читатели:

- Risk Manager (F-08 Post-trade).
- Collateral & Ledger (пересчёт позиций/балансов).
- Market Data Service (для live-отображения клиринговых цен и fill rate в UI).
- Observability & Reporting / Backtest & Replay (агрегация в ClickHouse).

**Кто кого дёргает.**

- **Scheduler** (внутри Matching Backend): раз в `batchIntervalMs` вызывает `Matching Backend.RunBatch(...)`.
- **Matching Backend (FOB Core)**: читает FlowOrder через `Collateral & Ledger.loadActiveFlowOrders`, запрашивает цены у `Market Data Service.getReferencePrices`, считает клиринг (`solveBatch`), пишет результаты в PostgreSQL через `Collateral & Ledger.saveFills/saveBatchResult` и в Kafka (`batch.outputs`, `fills`).
- **Дальнейшие потребители** (не часть F-04): Risk Manager и Settlement & Ledger читают `fills` и `batch.outputs` и обновляют риск/позиции (F-06/F-08). Web UI через Market Data Service и WebSocket получает обновлённые клиринговые цены и исполнение по ордерам.

---

# 5. Тестирование

## 5.1. Автоматические тесты

### Юнит-тесты солвера

Тестируем чистую функцию `solveBatch(batchId, flowOrders, referencePrices) → BatchResult, FillEvent[]` без PostgreSQL, Kafka и UI.

**1. Простые сценарии для проверки корректности решения.**

- **Один актив, симметричные buy/sell.** Два FlowOrder по BTCUSDT с одинаковым диапазоном цен и скоростями, один на покупку, другой на продажу.
  - Ожидание: `clear_prices["BTCUSDT"]` ≈ mid; `executed_rates` симметричны по модулю; сумма buy = сумма sell.
- **Два актива, простой портфель.** FlowOrder с портфелем «купить 1 BTC и продать 15 ETH» против обратного портфеля.
  - Ожидание: корректный клиринг по обоим активам; в `asset_legs` зеркальны между сторонами.

**2. Проверка диапазона клиринговых цен.**

Сгенерировать разные наборы `p_L, p_H` (узкий диапазон, широкий, почти не пересекающиеся), вызвать `solveBatch`, проверить инвариант:

∀ инструмент: min(p_L,i) ≤ clear_price ≤ max(p_H,i).

**3. Проверка `residual_norm` и условий остановки.**

- Идеально решаемый случай → `residual_norm` ≈ 0, малое число итераций, `stopped_by_tolerance=true`.
- Жёсткий случай (почти конфликтующие заявки) → solver упирается в `tolerance` или `max_iterations`.
  - `stopped_by_tolerance=true` если достигнута tolerance.
  - `stopped_by_max_iterations=true` если упёрся в `max_iterations`; `residual_norm > tolerance`.
- Дополнительно: `solve_time_ms` заполняется и в разумных интервалах.

### Интеграционные тесты (контейнеры + БД + Kafka)

Тестируем связку Matching Backend + Collateral & Ledger (PostgreSQL) + Market Data Service + Kafka + ClickHouse.

**1. Полный цикл данных.**

- В тестовом PostgreSQL — несколько записей в `flow_orders` со `status='active'`.
- Тестовый Market Data Service возвращает фиксированный mid/bid/ask.
- Запустить Matching Backend с тестовым Scheduler, который один раз вызовет `RunBatch(testBatchId, tsBatch)`.
- Проверить: в Kafka — одно сообщение в `batch.outputs` с `batch_id == testBatchId`; в `fills` — по FillEvent на каждую исполнившуюся заявку.
- В ClickHouse `fills` появляются строки соответствующие FillEvent, в `batch_results` — запись с `batch_id == testBatchId`.
- Инварианты: суммарный buy по активу = суммарному sell; суммы по `asset_legs` бьются между участниками; `liquidity_source` ожидаемый.

**2. Тест SLA по времени решения.**

- Сгенерировать 1000 активных FlowOrder с реалистичными параметрами.
- Запустить несколько батчей подряд, для каждого: измерить `solve_time_ms` из `BatchResult` и фактическое время `RunBatch`.
- Критерии прохождения: p95 ≤ 500 ms, p99 ≤ 1000 ms; доля батчей с `residual_norm > tolerance` ≤ 1%.

### Тест-кейсы в табличном виде

**Таблица 1. Симметричные buy/sell, один актив.**

| Test | FlowOrder 1 | FlowOrder 2 | RefPrice | Ожидание |
| --- | --- | --- | --- | --- |
| U1 | buy, p_L=99, p_H=101, q_rate=1, Q_max=10 | sell, p_L=99, p_H=101, q_rate=1, Q_max=10 | mid=100, bid=99, ask=101 | clear=100; rate1=1; rate2=−1; total buy = total sell |
| U2 | buy, q_rate=2, Q_max=10 | sell, q_rate=1, Q_max=10 | mid=100 | clear≈100; rate1=1; rate2=−1 (match ограничен меньшим q_rate) |
| U3 | buy, Q_max=2 | sell, Q_max=10 | mid=100 | clear=100; qty1=2 (выбил Q_max1), qty2=2; статус1=FILLED, статус2=ACTIVE |

**Таблица 2. Несимметричные диапазоны цен.**

| Test | FlowOrder 1 | FlowOrder 2 | RefPrice | Ожидание |
| --- | --- | --- | --- | --- |
| U4 | buy, p_L=99, p_H=100 | sell, p_L=100, p_H=101 | mid=100 | clear=100; rate1=1; rate2=−1 |
| U5 | buy, p_L=99, p_H=100 | sell, p_L=101, p_H=102 | mid=100 | clear ∈ допустимом диапазоне, но `executed_rates`≈0 (нет реального пересечения) |

**Таблица 3. Портфельный 2-активный ордер.**

| Test | A | B | RefPrice (BTC, ETH) | Ожидание |
| --- | --- | --- | --- | --- |
| U6 | buy: +1 BTC, −15 ETH | sell: −1 BTC, +15 ETH | BTC=60000, ETH=4000 | `asset_legs` зеркальны: A {+X BTC, −15X ETH}; B {−X BTC, +15X ETH}; суммы в системе = 0 |
| U7 | A как в U6, B = два FlowOrder (BTC и ETH отдельно), эквивалентные портфелю | те же | Solver матчит портфель A с набором ног B; сумма legs по всем fills = U6 |

**Таблица 4. Сходимость / несходимость.**

| Test | Описание | solver_config | Ожидание |
| --- | --- | --- | --- |
| U8 | Симметричный U1 | tolerance=1e-6, max_iterations=1000 | `residual_norm ≤ 1e-6`; iterations ≪ max; `stopped_by_tolerance=true` |
| U9 | Сконструированный жёсткий кейс | tolerance=1e-12, max_iterations=10 | iterations == 10; `residual_norm > tolerance`; `stopped_by_max_iterations=true` |

**Таблица 5. Инвариант по диапазону цен.**

| Test | Набор FlowOrder | Ref mid | Ожидание |
| --- | --- | --- | --- |
| U10 | Сгенерированный набор p_L/p_H | mid=100 | для всех инструментов: min(p_L) ≤ clear_price ≤ max(p_H) |

**Таблица 6. SLA solve_time_ms по размеру батча.**

| Размер батча (N) | Цель p95 | Жёсткий предел p99 | Комментарий |
| --- | --- | --- | --- |
| ≤ 50 | ≤ 20 ms | ≤ 50 ms | Малые батчи, dev/debug |
| 51–200 | ≤ 50 ms | ≤ 100 ms | MVP, нормальная нагрузка |
| 201–500 | ≤ 120 ms | ≤ 200 ms | Верхняя граница MVP |
| 501–1000 | ≤ 250 ms | ≤ 400 ms | Stress-режим / будущая оптимизация |
| > 1000 | не нормируется в MVP | — | Профилирование, планирование оптимизаций |

**Критерий прохождения SLA для nightly-тестов:** для каждого размера батча N в 1000 прогонов:

- p95 `solve_time_ms` ≤ цель;
- p99 ≤ жёсткий предел;
- доля батчей с `residual_norm > tolerance` ≤ 1%.

Alert-порог для production: если p95 `solve_time_ms` за последние 5 минут превышает SLA для актуального диапазона N, Risk/Observability поднимает предупреждение.

## 5.2. Ручные тесты

### Функциональные

1. Завести 2–3 demo-аккаунта (Auth & Identity).
2. Через Web UI / API Gateway создать набор понятных FlowOrder:
   - Клиент A: buy BTCUSDT, узкий диапазон вокруг mid, небольшой объём.
   - Клиент B: sell BTCUSDT в том же диапазоне и объёме.
   - Опционально: Клиент C с заявкой, которая не должна исполниться (слишком узкий/смещённый диапазон).
3. Дождаться одного батча.
4. Проверить через UI и через ClickHouse/PostgreSQL:
   - У A и B появились FillEvent с зеркальными объёмами.
   - `clear_price` по BTCUSDT близок к mid и попадает в объединённый интервал их p_L/p_H.
   - Заявка C осталась без исполнения (`exec_qty=0`, статус `active`).

Повторить с портфельной заявкой: покупка/продажа портфеля (BTC+ETH), проверить пропорции в обеих ногах.

### Диагностические

1. Открыть экран «Диагностика клиринга».
2. После выполнения батча убедиться, что отображаются ключевые поля BatchResult: `batch_id`, время, `clear_prices`, `executed_rates`, `residual_norm`, `solve_time_ms`, флаги из `solver_diagnostics`.
3. Проверить, что при выборе конкретного батча UI позволяет провалиться в список FillEvent, отфильтровать fills по клиентам/инструментам, увидеть параметры участвовавших FlowOrder.

Полезно воспроизвести один из юнит-кейсов (U1/U2) и проверить, что экран диагностики показывает ровно то, что ожидается.

### Негативные (нестабильное решение)

1. Подготовить специальный набор FlowOrder с почти конфликтующими диапазонами и сильно разными `q_rate/Q_max`.
2. В `solver_config` временно установить жёсткие параметры: `tolerance=1e-12`, `max_iterations=10`.
3. Запустить батч и посмотреть:
   - `residual_norm` заметно выше обычных значений.
   - В `solver_diagnostics` видно, что остановка по `max_iterations`.
   - `solve_time_ms` выше обычного, но в допустимых пределах.
4. Проверить реакцию системы:
   - На экране диагностики `residual_norm` подсвечен.
   - При превышении порога (например, `residual_norm > 0.01`) Observability & Reporting формирует alert.
   - При критическом `residual_norm` Risk Manager может выставить флаг kill-switch или сформировать risk_event.

Дополнительно: постепенно усложнять входные FlowOrder, смотреть, с какого момента диагностика/alerts меняется — чтобы команда понимала зону риска для production.

---

# 6. Definition of Done

Чек-лист DoD для F-04 «Batch Clearing Cycle» (для Jira):

1. [ ] Реализован `solveBatch(...)` в Matching Backend (FOB Core); возвращает `BatchResult` и `FillEvent` в формате BRD.
2. [ ] Пройден набор юнит-тестов солвера (U1–U10).
3. [ ] Matching Backend читает активные FlowOrder из PostgreSQL (`flow_orders WHERE status='active'` + окно времени).
4. [ ] Matching Backend запрашивает reference prices у Market Data Service и использует их в `solveBatch`.
5. [ ] После батча публикуются события в Kafka: `batch.outputs` (BatchResult) и `fills` (FillEvent).
6. [ ] ClickHouse принимает и сохраняет `batch.outputs` и `fills` в таблицы `batch_results` и `fills`.
7. [ ] Пройден интеграционный тест «полный цикл»: FlowOrder → RunBatch → Kafka → ClickHouse, с проверкой инвариантов (баланс объёмов, диапазон цен).
8. [ ] Пройден SLA-тест: p95/p99 `solve_time_ms` для целевых размеров батча укладываются в согласованные пороги (см. Таблицу 6).
9. [ ] В UI доступен экран «Диагностика клиринга» с отображением `batch_id`, `clear_prices`, `executed_rates`, `residual_norm`, `solve_time_ms` и списка FillEvent.
10. [ ] Настроены метрики и алерты по `solve_time_ms` и `residual_norm`; есть краткий операторский чек-лист «что делать при отклонениях».

---

> **Конец архивной копии.** Этот файл должен оставаться неизменяемым. Любые правки, уточнения, расширения — через новый incoming-документ с новым `IN-NNN`.
