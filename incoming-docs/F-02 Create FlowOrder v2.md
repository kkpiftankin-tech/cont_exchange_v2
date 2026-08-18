# F‑02. Создание одноактивной потоковой заявки (Flow Order)

***

## 1. Общая информация

### 1.1. Понятия и определения

#### Базовые сущности Flow Order

- **Flow Order (потоковая заявка)**
  Заявка на непрерывное исполнение определённого объёма актива с заданной скоростью в рамках ценового диапазона. В отличие от классического limit order, Flow Order задаёт не дискретный объём на фиксированной цене, а непрерывный поток: скорость \(q\) (tokens/sec), ценовой коридор \([p_L, p_H]\), максимальный объём \(Q_{max}\), и окно исполнения \([t_{start}, t_{end}]\).[^1]

- **Continuous Scaled Limit Order (CSLO)**
  Академическое название потоковой заявки из работ Kyle и Lee (2017). Параметры CSLO: \(P_L\) — нижняя цена, \(P_H\) — верхняя цена, \(Q\) — максимальный объём, \(U\) — скорость (tokens/sec). FlowOrder в системе FOB — программная реализация CSLO.[^1]

- **FlowOrder (объект данных)**
  Запись в PostgreSQL и доменная модель Matching Backend, содержащая:[^1]
  - `orderid` — UUID, первичный ключ.
  - `userid` — UUID, FK→users.
  - `providertype` — ENUM: `ui`, `api`, `provider`, `internal`.
  - `providerid` — VARCHAR NULL, ID источника заявки.
  - `symbol` — VARCHAR (например, BTCUSDT).
  - `side` — ENUM: `buy`, `sell`.
  - `portfolioweights` — JSONB NULL (NULL для одноактивной заявки F‑02).
  - `plow` — NUMERIC, нижняя граница цены \(p_L\).
  - `phigh` — NUMERIC, верхняя граница цены \(p_H\).
  - `qrate` — NUMERIC, скорость исполнения (units/sec).
  - `qmax` — NUMERIC, максимальный суммарный объём.
  - `filledcum` — NUMERIC, кумулятивно исполненный объём.
  - `timeinforce` — ENUM: `GTC`, `GTD`, `IOC`.
  - `windowstart` — TIMESTAMPTZ, начало окна исполнения.
  - `windowend` — TIMESTAMPTZ NULL, конец окна.
  - `status` — ENUM: `new`, `active`, `partiallyfilled`, `filled`, `cancelled`, `expired`, `liquidated`.
  - `createdat` — TIMESTAMPTZ.
  - `updatedat` — TIMESTAMPTZ.

***

#### Превью и метрики исполнения

- **VWAP (Volume-Weighted Average Price)**
  Средневзвешенная по объёму цена исполнения:
  $\text{VWAP} = \frac{\sum_i p_i \cdot q_i}{\sum_i q_i}$
  где $p_i$ — цена, $q_i$ — объём каждого fill. Используется для оценки качества исполнения.[^1]

- **Implementation Shortfall (IS)**
  Разница между стоимостью приобретения актива по «идеальной» цене $P_0$ (цена на момент принятия решения) и фактической стоимостью исполнения. IS включает как cost of delay, так и market impact.[^1]

- **Доверительный интервал IS**
  При превью заявки UI показывает полосу неопределённости (95‑й перцентиль) вокруг ожидаемого IS. Чем быстрее трейдер торгует (\(q \uparrow\)), тем выше ожидаемый IS, но ниже дисперсия; чем медленнее (\(q \downarrow\)), тем ниже ожидаемый IS, но выше волатильность результата (The Funnel).[^1]

- **Price Impact (ценовое воздействие)**
  Изменение рыночной цены, вызванное торговой активностью. Разделяется на:
  - **temporary** — функция скорости \(v\), пропадает после исполнения;
  - **permanent** — линейная функция количества \(x\), отражает сдвиг инвентарной позиции маркет‑мейкера.[^1]

***

#### Execution Profile

- **Execution Profile / Schedule**
  Профиль скорости исполнения \(v(t)\) во времени. MVP поддерживает два формата:[^1]
  - **SEGMENTED** — явное задание временных сегментов с `tstart`, `tend`, `targetnotional`, `priceconstraint`, `participationcap` для каждого.
  - **PARAMETRIC** — preset‑профили: `TWAP`, `AlmgrenChrissIS`, `POV` и т.д. с дополнительными `params`.

- **Time In Force**
  Срок действия заявки:[^1]
  - `GTC` (Good Till Cancel) — до отмены.
  - `GTD` (Good Till Date) — до указанного `windowend`.
  - `IOC` (Immediate or Cancel) — исполнить максимум за один батч, остаток отменить.

***

#### Статусы FlowOrder

| Статус | Описание |
|--------|----------|
| `new` | Заявка создана, ожидает pre‑trade проверки |
| `active` | Прошла pre‑trade, участвует в батч‑клиринге [^1] |
| `partiallyfilled` | Часть объёма исполнена (`filledcum > 0`, `filledcum < qmax`) |
| `filled` | Полностью исполнена (`filledcum >= qmax`) |
| `cancelled` | Отменена трейдером (F‑03) или системой |
| `expired` | `windowend` истёк без полного исполнения |
| `liquidated` | Позиция ликвидирована Risk Manager (F‑08) |

***

#### Транспорт и хранилища

- **floworders (таблица PostgreSQL)**
  OLTP‑таблица, из которой Matching Backend собирает активные заявки для батч‑клиринга (F‑04). Matching Backend обновляет `filledcum` и `status` после каждого батча.[^2][^1]

- **Kafka `orders.normalized`**
  Топик, куда Matching Backend публикует новые/обновлённые FlowOrder после активации. Потребители: Market Data Service, Risk Manager, Web UI (через WebSocket).[^1]

- **fills (таблица ClickHouse)**
  Аналитическая таблица FillEvent, куда попадают результаты исполнения: `fillid`, `batchid`, `orderid`, `execqty`, `execprice`, `liquiditysource`, `fees`, `timestamp`.[^2][^1]

***

### 1.2. Краткое описание

Фича отвечает за полный жизненный цикл создания одноактивной потоковой заявки: от ввода параметров трейдером через UI/API, через расчёт превью (прогнозируемый VWAP, доверительный интервал IS), pre‑trade проверку (F‑07), до активации заявки и начала участия в батч‑клиринге (F‑04).[^1]

### 1.3. Область применения

- Web UI Trading Frontend (React) — форма ввода параметров и отображение превью.
- API Gateway / Provider‑Client Backend — HTTP REST API для приёма заявок.
- Matching Backend (C++/Go) — валидация, pre‑trade вызов, запись в PostgreSQL, публикация в Kafka.
- Risk Manager (Go) — синхронная pre‑trade проверка (F‑07).
- Все одноактивные инструменты: single‑asset FlowOrder с `portfolioweights = NULL`.[^1]

### 1.4. Заинтересованные стороны

- Трейдеры/клиенты — создают заявки, видят превью и статус.
- Провайдеры ликвидности, в том числе Exchange‑provider (Provider–Exchange блок) — создают заявки через API (`providertype = provider`). Exchange‑provider использует FillEvent по своим FlowOrder для хеджирования внешней позиции на LOB‑биржах.[^2]
- Команды Matching, Risk, Frontend, Provider‑Client.

***

## 2. Бизнес‑часть (BRD)

### 2.1. Назначение и цели

Функция предназначена для:[^1]
- предоставления трейдеру интуитивного интерфейса создания потоковой заявки с параметрами \(p_L, p_H, q, Q_{max}\);
- расчёта и отображения превью исполнения (прогнозируемый VWAP, доверительный интервал IS) перед подтверждением;
- валидации заявки через pre‑trade риск‑контроль (F‑07);
- активации заявки и включения её в ближайший цикл батч‑клиринга (F‑04).

**Цели:**
- Каждая созданная FlowOrder содержит полный набор параметров CSLO.[^1]
- Превью показывает трейдеру ожидаемые характеристики исполнения до подтверждения.
- Время от подтверждения до активации (включая pre‑trade) не превышает 100 ms p95.
- Активная заявка участвует в ближайшем батче (`batchintervalms`).[^2]

### 2.2. Предусловия

- Пользователь авторизован (F‑01), роль: `client`, `provider` или `demo`.
- Счёт пользователя создан, баланс по quote‑активу (например, USDT) достаточен.
- Инструмент (symbol) существует и активен в системе.
- Risk Manager доступен для pre‑trade проверки (F‑07).
- Matching Backend запущен, текущая конфигурация солвера (`solverconfig`) активна.[^2][^1]

### 2.3. Основной успешный сценарий

#### 2.3.1. Диаграмма последовательности (бизнес‑уровень)

Участники:
- Трейдер (или Exchange‑provider через API).
- Система биржи (FOB).

```mermaid
sequenceDiagram
    participant T as Трейдер / Exchange-provider
    participant S as Система биржи FOB

    T->>S: Выбрать инструмент (BTCUSDT)
    T->>S: Задать параметры: pL, pH, q, Qmax, Time In Force
    S->>S: Рассчитать превью: прогнозируемый VWAP, доверительный интервал IS
    S-->>T: Показать превью (VWAP, IS ± CI95)

    T->>S: Подтвердить создание заявки
    S->>S: Pre‑trade проверка (F‑07)

    alt Pre‑trade approve
        S->>S: Активировать FlowOrder (status = active)
        S-->>T: Статус «Активна»
        S->>S: Заявка участвует в ближайшем батч‑клиринге (F‑04)
        S-->>T: Начало частичных исполнений (fills)
    else Pre‑trade reject
        S-->>T: Причина отказа
    else Pre‑trade throttle
        S->>S: Активировать с уменьшенными параметрами
        S-->>T: Статус «Активна» + скорректированные параметры
    end
```

#### 2.3.2. Описание действий

1. Трейдер открывает торговый экран в Web UI, выбирает инструмент (BTCUSDT).[^1]
2. Трейдер задаёт параметры заявки: \(p_L\), \(p_H\), скорость \(q\) (tokens/sec), максимальный объём \(Q_{max}\), Time In Force (GTC/GTD/IOC), опционально — окно исполнения.[^1]
3. UI отправляет preview‑запрос: система на основе текущего состояния order book (clearing prices, residual demand/supply) рассчитывает прогнозируемый VWAP и доверительный интервал IS (95‑й перцентиль).[^1]
4. Трейдер видит превью и подтверждает создание заявки.
5. Web UI отправляет HTTP POST `/api/v1/orders/flow` через API Gateway.[^1]
6. API Gateway проверяет аутентификацию, rate limits, маршрутизирует в Matching Backend.
7. Matching Backend формирует FlowOrder с `status = new`, вызывает Risk Manager `CheckOrder` (F‑07).[^1]
8. При `approve`: Matching Backend записывает FlowOrder в PostgreSQL (`status = active`), публикует в Kafka `orders.normalized`.[^1]
9. Web UI через WebSocket получает подтверждение: «Заявка активна».
10. В ближайшем батче (F‑04) Matching Backend включает эту FlowOrder в набор активных заявок, солвер рассчитывает clearing prices и fills.[^2][^1]
11. Трейдер видит первые частичные исполнения, обновлённый `filledcum` и статус `partiallyfilled`.
12. **Exchange‑provider** (Provider–Exchange блок) читает FillEvent по своим FlowOrder и инициирует хедж‑сделки на внешних LOB‑биржах через Venue Execution Adapter.[^2]

### 2.4. Дополнительные сценарии

- **Pre‑trade reject** → FlowOrder не активируется. Трейдер видит причину отказа (например, `maxnotional exceeded`). Может скорректировать параметры и повторить.[^1]
- **Pre‑trade throttle** → FlowOrder активируется с уменьшенными `qrate` и/или `qmax`. Трейдер видит исходные и скорректированные параметры.
- **Demo‑пользователь** → полный цикл с mock‑балансами и mock‑исполнениями. Не влияет на реальный order book.[^1]
- **Создание через API** (`providertype = api` или `provider`) → тот же цикл без UI, ответ — JSON с FlowOrder или ошибкой.
- **IOC заявка** → участвует только в одном ближайшем батче. Неисполненный остаток автоматически отменяется (`status = cancelled`).[^1]
- **GTD заявка** → при наступлении `windowend` неисполненный остаток получает `status = expired`.
- **Инструмент отсутствует в whitelist** → мгновенный reject на этапе pre‑trade.
- **Недостаточный баланс** → reject с причиной «Insufficient collateral».
- **Exchange‑provider отправляет заявку через API** (`providertype = provider`) → тот же пайплайн, `providerid` заполнен; после получения FillEvent Exchange‑provider хеджирует открытую позицию на LOB‑биржах.[^2]

### 2.5. UX / UI

```
+--------------------------------------------------------------+
|                     Web UI — Торговый экран                   |
+-----------------------------+--------------------------------+
| Форма создания FlowOrder    | Превью исполнения              |
|                             |                                |
| Инструмент: [BTCUSDT ▼]    | Прогноз VWAP: 64,850 USDT     |
| Сторона:    [Buy ▼]        | Ожидаемый IS: −42 USDT        |
|                             | 95% CI IS: [−120, +36] USDT   |
| pL (мин. цена):    |                                |
| pH (макс. цена):   | Price Impact (temp): 12 USDT  |
| q (скорость): [0.5 tok/s]  | Price Impact (perm): 30 USDT  |
| Qmax (макс. объём): [10.0] | Прогноз fill time: 20 сек     |
|                             |                                |
| Time In Force: [GTC ▼]     | ╔═══════════════════════╗     |
| Окно до: [опционально]     | ║   The Funnel Chart     ║     |
|                             | ║ (IS vs Speed tradeoff) ║     |
| [Рассчитать превью]        | ║   ● текущий q=0.5     ║     |
| [Создать заявку]            | ╚═══════════════════════╝     |
+-----------------------------+--------------------------------+
| Статус заявки (после создания)                               |
|                                                              |
| При approve:                                                 |
|   Зелёная метка «Активна» | orderid: a1b2c3...             |
|   filledcum: 0.0 → обновляется в реальном времени           |
|                                                              |
| При reject:                                                  |
|   Красная метка «Отклонена» + причина                       |
|                                                              |
| При throttle:                                                |
|   Жёлтая метка «Скорректирована»                            |
|   было q=0.5, стало q=0.33; было Qmax=10, стало Qmax=6.5   |
+--------------------------------------------------------------+
| Лента исполнений (fills) — обновляется через WebSocket       |
|                                                              |
| batchid       | execqty | execprice | fees  | source        |
| batch-001...  | 0.3     | 64,950    | 0.12  | internal      |
| batch-002...  | 0.2     | 65,010    | 0.08  | cexhedge      |
+--------------------------------------------------------------+
```

### 2.6. Критерии успеха

- Трейдер (или Exchange‑provider) может создать одноактивную FlowOrder через UI и API с полным набором параметров.[^1]
- Превью VWAP и IS CI показываются до подтверждения.
- Время от подтверждения до активации (включая pre‑trade) — p95 < 100 ms.
- Активная заявка участвует в ближайшем батче.[^2]
- Статус и fills обновляются в реальном времени через WebSocket.
- При reject/throttle трейдер видит конкретную причину.
- Exchange‑provider получает FillEvent и может использовать его для хеджирования на LOB‑биржах.[^2]

***

## 3. Требования

### 3.1. Функциональные требования

F2‑1. Web UI должен предоставлять форму создания одноактивной FlowOrder с полями: symbol, side, \(p_L\), \(p_H\), \(q\), \(Q_{max}\), Time In Force, опционально windowstart/windowend.[^1]

F2‑2. При заполнении параметров UI должен запрашивать превью и отображать прогнозируемый VWAP, ожидаемый IS, 95‑й перцентиль IS (доверительный интервал).[^1]

F2‑3. Превью должно рассчитываться на основе текущего состояния order book (residual demand/supply curves) и параметров price impact из Matching Backend / Market Data Service.[^1]

F2‑4. Трейдер должен иметь возможность подтвердить создание заявки после просмотра превью.

F2‑5. API Gateway должен принимать HTTP POST `/api/v1/orders/flow` с JSON‑телом, содержащим параметры FlowOrder.[^1]

F2‑6. Matching Backend должен синхронно вызвать Risk Manager `CheckOrder` (F‑07) перед активацией.[^1]

F2‑7. При `approve` Matching Backend должен записать FlowOrder в PostgreSQL (`status = active`) и опубликовать в Kafka `orders.normalized`.[^1]

F2‑8. При `reject` Matching Backend должен вернуть HTTP 403 с причиной отказа. FlowOrder не записывается как active.[^1]

F2‑9. При `throttle` Matching Backend должен записать FlowOrder с `adjustedParams` (скорректированные `qrate`/`qmax`) и опубликовать в Kafka.

F2‑10. Web UI должен получать подтверждение активации / reject / throttle через WebSocket или HTTP response и отображать соответствующий статус.[^1]

F2‑11. Активная FlowOrder должна участвовать в ближайшем цикле батч‑клиринга (F‑04). Matching Backend собирает все active FlowOrder из PostgreSQL для формирования BatchRequest.[^2]

F2‑12. После каждого батча `filledcum` и `status` FlowOrder должны обновляться в PostgreSQL на основе FillEvent.[^2]

F2‑13. Web UI должен отображать ленту fills (batchid, execqty, execprice, fees, liquiditysource) с обновлением через WebSocket.[^1]

F2‑14. Для FlowOrder с `timeinforce = IOC` неисполненный после одного батча остаток должен получить `status = cancelled`.[^1]

F2‑15. Для FlowOrder с `timeinforce = GTD` по достижении `windowend` неисполненный остаток должен получить `status = expired`.[^1]

F2‑16. Одноактивная FlowOrder должна иметь `portfolioweights = NULL`.[^1]

F2‑17. **[ДОБАВЛЕНО]** Exchange‑provider (Provider–Exchange блок) должен иметь возможность создавать FlowOrder через API с `providertype = provider`. После получения FillEvent из батч‑клиринга Exchange‑provider использует данные о своей исполненной позиции (`execqty`, `execprice`, `liquiditysource`) для принятия решения о хеджировании на внешних LOB‑биржах.[^2]

### 3.2. Нефункциональные требования

- Время от HTTP POST до ответа с решением (включая pre‑trade) — p95 < 100 ms, p99 < 500 ms.[^1]
- Matching Backend должен обрабатывать до 5000 создаваемых FlowOrder/секунду при штатной нагрузке.
- Превью (VWAP, IS CI) должно рассчитываться менее чем за 50 ms.
- WebSocket‑уведомления о fills и статусе должны доставляться с задержкой не более 200 ms после записи FillEvent.[^2]
- При недоступности Risk Manager или PostgreSQL — fail‑safe reject, а не пропуск pre‑trade проверки.
- Данные FlowOrder хранятся в PostgreSQL с ACID‑гарантиями.[^1]

***

## 4. Техническая архитектура (TRD)

### 4.1. Состав компонентов

- **Web UI** (React) — форма ввода FlowOrder, отображение превью, статуса, fills. WebSocket‑подписка на обновления.
- **API Gateway / Provider‑Client** (Go) — HTTP REST API (`POST /api/v1/orders/flow`, `GET /api/v1/orders/active`), аутентификация, rate limiting, маршрутизация.[^1]
- **Matching Backend** (C++/Go) — валидация параметров, вызов Risk Manager, запись FlowOrder в PostgreSQL, публикация в Kafka, batch scheduler.[^2]
- **Risk Manager** (Go) — синхронная pre‑trade проверка `CheckOrder` (F‑07).
- **PostgreSQL** — таблица `floworders` (OLTP), `accounts`, `positions`, `risklimits`.[^1]
- **Kafka** — топик `orders.normalized` для публикации активных FlowOrder; топик `batch.outputs` / `fills` для доставки FillEvent.[^2]
- **Market Data Service** — предоставляет clearing prices, residual curves для расчёта превью.[^1]
- **ClickHouse** — таблица `fills` для аналитического хранения FillEvent.
- **Exchange‑provider (Provider–Exchange блок)** — подписывается на FillEvent по своим FlowOrder (`providerid`); после их получения принимает решение о хеджировании и отправляет хедж‑ордера через Venue Execution Adapter.[^2]

### 4.2. Диаграмма последовательности (техническая)

**Сценарий: Создание одноактивной FlowOrder (approve)**

```mermaid
sequenceDiagram
    participant U  as Трейдер
    participant UI as Web UI
    participant GW as API Gateway
    participant MB as Matching Backend
    participant RM as Risk Manager
    participant MD as Market Data Service
    participant DB as PostgreSQL
    participant K  as Kafka
    participant WS as WebSocket
    participant EP as Exchange-provider

    U->>UI: Выбрать BTCUSDT, задать pL, pH, q, Qmax, TIF
    UI->>GW: GET /api/v1/preview/flow {symbol, side, plow, phigh, qrate, qmax}
    GW->>MB: Проксировать preview request
    MB->>MD: Запросить текущие clearing prices, residual curves
    MD-->>MB: Clearing prices, demand/supply data
    MB->>MB: Рассчитать expected VWAP, E[IS], Var[IS], CI95
    MB-->>GW: PreviewResult {vwap, expectedIS, ci95low, ci95high, tempImpact, permImpact, estFillTime}
    GW-->>UI: PreviewResult
    UI-->>U: Показать превью (VWAP, IS ± CI95, The Funnel)

    U->>UI: Подтвердить создание заявки
    UI->>GW: POST /api/v1/orders/flow {symbol, side, plow, phigh, qrate, qmax, timeinforce}
    GW->>GW: Auth check, rate limit
    GW->>MB: Forward order creation request

    MB->>MB: Валидация параметров (plow < phigh, qrate > 0, qmax > 0)
    MB->>RM: gRPC CheckOrder(RiskCheckRequest)
    RM->>DB: SELECT risklimits, positions, accounts
    DB-->>RM: Лимиты, позиции, балансы
    RM->>RM: Проверка killswitch, whitelist, notional, position, leverage, rate
    RM-->>MB: RiskCheckResult(decision=approve)

    MB->>DB: INSERT INTO floworders (status=active, filledcum=0)
    DB-->>MB: OK
    MB->>K: Publish orders.normalized {FlowOrder}
    MB-->>GW: 201 Created {FlowOrder}
    GW-->>UI: 201 Created
    UI-->>U: Зелёная метка «Активна»

    Note over MB: Ближайший batch cycle (F‑04)
    MB->>DB: SELECT active floworders WHERE symbol=BTCUSDT
    MB->>MB: Solver: clearing prices, fills
    MB->>DB: UPDATE floworders SET filledcum += execqty, status = partiallyfilled
    MB->>K: Publish batch.outputs {FillEvent}
    K-->>WS: Push fill update
    WS-->>UI: FillEvent {orderid, execqty, execprice, fees}
    UI-->>U: Обновить ленту fills и filledcum

    Note over EP: Exchange-provider читает FillEvent по своим провайдерским заявкам
    K-->>EP: FillEvent {orderid, execqty, execprice, liquiditysource}
    EP->>EP: Рассчитать открытую позицию для хеджирования
    EP->>EP: Отправить хедж-ордер на LOB-биржу (Venue Execution Adapter)
```

### 4.3. Техническая интеграция

#### 4.3.1. Интеграции

- Web UI → API Gateway: HTTP REST (POST/GET), WebSocket для real‑time updates.[^1]
- API Gateway → Matching Backend: internal HTTP/gRPC proxy.
- Matching Backend → Risk Manager: синхронный gRPC `CheckOrder`.
- Matching Backend → PostgreSQL: CRUD `floworders` (INSERT, UPDATE status/filledcum).[^2]
- Matching Backend → Kafka: publish `orders.normalized`, `batch.outputs`, `fills`.[^2]
- Matching Backend → Market Data Service: запрос clearing prices для превью.[^1]
- Risk Manager → PostgreSQL: read `risklimits`, `positions`, `accounts`.
- Kafka → Web UI (WebSocket): push fills, status updates.
- Kafka → ClickHouse: ingestion `fills` через Kafka table engine.
- **[ДОБАВЛЕНО]** Kafka `batch.outputs` → Exchange‑provider: push FillEvent по провайдерским FlowOrder для хеджирования.[^2]

#### 4.3.2. Изменяемые объекты конфигурации

- Таблица `solverconfig` в PostgreSQL (`batchintervalms`, `maxiterations`, `epsilonliquidity`, `tolerance`, `feemodel`).[^2]
- Параметры price impact модели в Market Data Service (slope, half‑spread).
- Параметры preview расчёта (volatility estimate, permanent impact coefficient).[^1]

### 4.4. Способ реализации

#### Matching Backend (создание FlowOrder)

Основной метод: `CreateFlowOrder(req: CreateFlowOrderRequest)` → `(FlowOrder, error)`

1. `validate(req)` — проверить:
   - `req.plow < req.phigh` (ценовой диапазон корректен).
   - `req.qrate > 0` (скорость положительна).
   - `req.qmax > 0` (объём положителен).
   - `req.symbol` существует в системе.
   - `req.side` ∈ {`buy`, `sell`}.
   - `req.timeinforce` ∈ {`GTC`, `GTD`, `IOC`}.
   - Если `GTD` → `req.windowend` задан и > now().

2. `order = buildFlowOrder(req, userid, providertype)`
   - Сгенерировать `orderid` (UUID).
   - `portfolioweights = NULL` (одноактивная заявка).
   - `filledcum = 0`.
   - `status = new`.
   - `windowstart = now()`.
   - `windowend = req.windowend` (или NULL для GTC).

3. `riskResult = RiskManager.CheckOrder(buildRiskCheckRequest(order))` — синхронный gRPC (F‑07).

4. Если `riskResult.decision == approve`:
   - `order.status = active`.
   - `DB.insertFlowOrder(order)`.
   - `Kafka.publish("orders.normalized", order)`.
   - return `(order, nil)`.

5. Если `riskResult.decision == throttle`:
   - `order = applyThrottle(order, riskResult.adjustedParams)`.
   - `order.status = active`.
   - `DB.insertFlowOrder(order)`.
   - `Kafka.publish("orders.normalized", order)`.
   - return `(order, nil)` с пометкой throttle.

6. Если `riskResult.decision == reject`:
   - return `(nil, RejectError(riskResult.reason))`.

#### Matching Backend (расчёт превью)

Метод: `ComputePreview(req: PreviewRequest)` → `PreviewResult`[^1]

1. `marketState = MarketDataService.getCurrentState(req.symbol)` — получить clearing prices, residual demand/supply.

2. Рассчитать temporary price impact:
   \[\Phi(v) = \alpha_0 + \alpha_1 \cdot v\]
   где \(\alpha_0\) — half‑spread, \(\alpha_1\) — slope residual curve.

3. Рассчитать permanent price impact:
   \[\gamma \cdot x, \quad x = Q_{max}\]

4. Рассчитать ожидаемый VWAP (для buy):
   \[E[\text{VWAP}] = P_0 + \Phi(q) + \frac{1}{2}\gamma \cdot Q_{max}\]

5. Рассчитать E[IS] и Var[IS]:
   \[E[\text{IS}] = \Phi(q) \cdot Q_{max} + \frac{1}{2}\gamma \cdot Q_{max}^2\]
   \[\text{Var}[\text{IS}] = q^2 \cdot \sigma^2 \cdot T^3 / 3, \quad T = Q_{max}/q\]

6. CI95: \([E[\text{IS}] - 1.96\sqrt{\text{Var}},\; E[\text{IS}] + 1.96\sqrt{\text{Var}}]\)

7. return `PreviewResult{vwap, expectedIS, ci95low, ci95high, tempImpact, permImpact, estFillTime}`.

***

#### JSON‑формат API

**Request: POST /api/v1/orders/flow**

```json
{
  "symbol": "BTCUSDT",
  "side": "buy",
  "plow": 59000.00,
  "phigh": 61000.00,
  "qrate": 0.5,
  "qmax": 10.0,
  "timeinforce": "GTC",
  "windowend": null
}
```

**Response: 201 Created (approve)**

```json
{
  "orderid": "a1b2c3d4-e5f6-...",
  "userid": "user-007",
  "symbol": "BTCUSDT",
  "side": "buy",
  "plow": 59000.00,
  "phigh": 61000.00,
  "qrate": 0.5,
  "qmax": 10.0,
  "filledcum": 0.0,
  "timeinforce": "GTC",
  "status": "active",
  "riskDecision": "approve",
  "windowstart": "2026-03-17T08:30:00Z",
  "windowend": null,
  "createdat": "2026-03-17T08:30:00Z"
}
```

**Response: 201 Created (throttle)**

```json
{
  "orderid": "a1b2c3d4-e5f6-...",
  "userid": "user-007",
  "symbol": "BTCUSDT",
  "side": "buy",
  "plow": 59000.00,
  "phigh": 61000.00,
  "qrate": 0.33,
  "qmax": 10.0,
  "filledcum": 0.0,
  "timeinforce": "GTC",
  "status": "active",
  "riskDecision": "throttle",
  "riskReason": "maxnotional exceeded: adjusted qrate 0.50 -> 0.33",
  "originalQrate": 0.5,
  "windowstart": "2026-03-17T08:30:00Z",
  "windowend": null,
  "createdat": "2026-03-17T08:30:00Z"
}
```

**Response: 403 Forbidden (reject)**

```json
{
  "error": "pretrade_reject",
  "reason": "maxleverage exceeded: projected 12.5x > limit 10.0x",
  "orderid": null
}
```

**Request: GET /api/v1/preview/flow**

Query params: `symbol=BTCUSDT&side=buy&plow=59000&phigh=61000&qrate=0.5&qmax=10`

**Response: 200 OK**

```json
{
  "symbol": "BTCUSDT",
  "side": "buy",
  "expectedVwap": 64850.00,
  "expectedIS": -42.00,
  "ci95low": -120.00,
  "ci95high": 36.00,
  "temporaryImpact": 12.00,
  "permanentImpact": 30.00,
  "estimatedFillTimeSec": 20.0,
  "currentMidPrice": 64800.00
}
```

***

## 5. Тестирование

### 5.1. Автоматические тесты

#### Юнит‑тесты Matching Backend

1. **Валидация: корректные параметры** — `plow=59000`, `phigh=61000`, `qrate=0.5`, `qmax=10`, `side=buy`, `TIF=GTC` → проходит без ошибок.
2. **Валидация: plow >= phigh** — `plow=61000`, `phigh=59000` → ошибка «plow must be less than phigh».
3. **Валидация: qrate <= 0** → ошибка «qrate must be positive».
4. **Валидация: qmax <= 0** → ошибка «qmax must be positive».
5. **Валидация: GTD без windowend** → ошибка «windowend required for GTD orders».
6. **Approve** — mock Risk Manager возвращает approve → FlowOrder в DB со `status=active`, `filledcum=0`, `portfolioweights=NULL`.
7. **Reject** — mock Risk Manager возвращает reject → запись в `floworders` отсутствует, HTTP 403.
8. **Throttle** — mock Risk Manager возвращает `throttle` с `adjustedQrate=0.33` → FlowOrder в DB с `qrate=0.33`.
9. **Kafka publish при approve** → сообщение в `orders.normalized` содержит полный FlowOrder со `status=active`.
10. **Kafka не publish при reject** → в `orders.normalized` нет нового сообщения.[^1]

#### Юнит‑тесты Preview

11. **Превью VWAP расчёт** — задать `midPrice=65000`, `alpha1=193.48`, `gamma=2.89`, `qrate=0.5`, `qmax=10` → `expectedVwap` и `expectedIS` соответствуют формулам.
12. **Превью CI95** — задать `sigma=7.2963` → `ci95low` и `ci95high` корректно через `1.96 * sqrt(Var)`.[^1]

#### Интеграционные тесты

1. **Полный цикл approve** (UI → DB → Kafka) → в `floworders` запись со `status=active`; в Kafka `orders.normalized` сообщение; HTTP 201.
2. **Полный цикл reject** → в `floworders` нет записи active; в Kafka `risk.alerts` — `pretrade‑reject`.
3. **Полный цикл throttle** → в `floworders` запись с adjusted `qrate`.
4. **FlowOrder → batch → fill** → создать FlowOrder (approve), дождаться batch cycle, проверить `filledcum > 0`, FillEvent в ClickHouse `fills`.[^2]
5. **IOC: отмена после одного батча** → `filledcum` обновлён, оставшийся объём — `status=cancelled`.
6. **GTD: expiration** → создать FlowOrder с `GTD`, `windowend=now()+5s`; через 6s проверить `status=expired`.
7. **Preview endpoint корректность** → HTTP 200, все поля числа, не null.
8. **WebSocket push fills** → создать FlowOrder, подписаться на WebSocket; после batch проверить получение FillEvent с корректным `orderid`.
9. **[ДОБАВЛЕНО] Exchange‑provider читает FillEvent** → создать FlowOrder с `providertype=provider`; после batch проверить доставку FillEvent в консьюмер Exchange‑provider с корректным `providerid`.[^2]

### 5.2. Нагрузочные тесты

- **Пропускная способность**: 5000 HTTP POST `/orders/flow` в секунду при p95 latency < 100 ms.
- **Preview**: 10 000 GET `/preview/flow` в секунду при p95 < 50 ms.
- **WebSocket fan‑out**: 1000 одновременных подписок, fills доставляются с задержкой < 200 ms.[^1]

### 5.3. Ручные тесты

- **Создание через UI**: ввести параметры → увидеть превью → подтвердить → «Активна» → видеть fills.
- **Ошибки валидации**: ввести `pL > pH` → UI показывает inline‑ошибку до отправки.
- **Reject в UI**: заявка выше лимитов → красная метка с конкретной причиной.
- **Throttle в UI**: жёлтая метка, отображение исходных и скорректированных параметров.
- **Demo‑пользователь**: полный цикл с mock‑данными.[^1]

***

## 6. Definition of Done (чек‑лист для Jira)

- [ ] Web UI предоставляет форму создания одноактивной FlowOrder со всеми параметрами (symbol, side, pL, pH, q, Qmax, TIF, windowend).
- [ ] Превью (VWAP, IS, CI95) рассчитывается и отображается до подтверждения.
- [ ] API endpoint `POST /api/v1/orders/flow` принимает и валидирует параметры.
- [ ] API endpoint `GET /api/v1/preview/flow` возвращает PreviewResult.
- [ ] Matching Backend синхронно вызывает Risk Manager `CheckOrder` перед активацией.
- [ ] При approve → FlowOrder записана в `floworders` (status=active), опубликована в `orders.normalized`.
- [ ] При reject → HTTP 403, причина отказа в UI, FlowOrder не активирована.
- [ ] При throttle → FlowOrder активирована с adjustedParams, трейдер видит коррекцию.
- [ ] Активная FlowOrder участвует в ближайшем batch cycle (F‑04).
- [ ] `filledcum` и `status` обновляются после каждого batch.
- [ ] Fills отображаются в UI в реальном времени через WebSocket.
- [ ] IOC: неисполненный остаток отменяется после одного batch.
- [ ] GTD: неисполненный остаток получает `expired` по истечении windowend.
- [ ] **[ДОБАВЛЕНО]** Exchange‑provider получает FillEvent по своим провайдерским FlowOrder через Kafka.
- [ ] Юнит‑тесты (12 кейсов) проходят для валидации, approve, reject, throttle, preview.
- [ ] Интеграционные тесты (9 кейсов, включая тест Exchange‑provider FillEvent) проходят end‑to‑end.
- [ ] Нагрузочный тест подтверждает SLA: p95 < 100 ms при 5000 req/s.
- [ ] Документация API (OpenAPI spec) обновлена и согласована.

***

## Приложение: реестр всех исправлений

| # | Раздел | Что было | Что стало | Причина |
|---|--------|----------|-----------|---------|
| 1 | 1.4 Заинтересованные стороны | Провайдеры ликвидности упомянуты только как «создают через API» | Явно добавлен **Exchange‑provider** как отдельная роль с пояснением: после получения FillEvent он хеджирует позицию на LOB‑биржах | Архитектурное решение, закреплённое в F‑04 и в предыдущем обсуждении блока Provider–Exchange |
| 2 | 2.3.2 Описание действий, шаг 12 | Отсутствовал | Добавлен шаг 12: Exchange‑provider читает FillEvent и инициирует хедж через Venue Execution Adapter | Связь F‑02 → F‑04 → хедж была явно зафиксирована в F‑04, но в F‑02 не упоминалась |
| 3 | 2.4 Дополнительные сценарии | Нет сценария для Exchange‑provider | Добавлен сценарий «Exchange‑provider отправляет заявку через API (`providertype = provider`)» | Соответствие реальному flow блока Provider–Exchange |
| 4 | 2.6 Критерии успеха | Нет критерия про Exchange‑provider | Добавлен критерий: «Exchange‑provider получает FillEvent и может использовать его для хеджирования» | Полнота критериев |
| 5 | 3.1 F2‑17 | Требование отсутствовало | Добавлено новое функциональное требование F2‑17 о поддержке Exchange‑provider как провайдера с `providertype = provider` и доставке FillEvent | Трассировка из архитектурного решения |
| 6 | 4.1 Состав компонентов | Exchange‑provider не упомянут | Добавлен как компонент с описанием: подписывается на FillEvent, решает о хедже, отправляет ордера через Venue Execution Adapter | Архитектурная полнота |
| 7 | 4.2 Диаграмма последовательности | Не было участника Exchange‑provider | Добавлен participant `EP as Exchange-provider` и шаги: получение FillEvent → расчёт позиции → хедж‑ордер | Визуализация полного flow |
| 8 | 4.3.1 Интеграции | Нет интеграции Kafka → Exchange‑provider | Добавлена строка: `Kafka batch.outputs → Exchange‑provider` | Полнота интеграционной карты |
| 9 | 5.1 Интеграционные тесты | 8 тестов | Добавлен тест №9: Exchange‑provider читает FillEvent | Покрытие нового требования F2‑17 |
| 10 | 6. DoD | 16 пунктов | Добавлен пункт: Exchange‑provider получает FillEvent через Kafka | Трассируемость требования до чек‑листа |
| 11 | Формулы IS, VWAP, Var | Использовались `$ $` (dollar signs) | Переведены в `\( \)` / `\[ \]` (LaTeX inline/block) согласно стандарту оформления Space | Соответствие правилам оформления формул |

---

## References

1. [F-02-Create-Taskl.md](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/5a35ae03-4ab7-45b6-b71a-ae33d7057d61/F-02-Create-Taskl.md?AWSAccessKeyId=ASIA2F3EMEYE4AG2GW4Y&Signature=ueGo%2FZgcU6y00cJwpGVN7ShhIY8%3D&x-amz-security-token=IQoJb3JpZ2luX2VjECYaCXVzLWVhc3QtMSJHMEUCIG11jVjjGAFhuGl%2FKxcMIErQiuwy0y6XQXwiI%2BKzVpQhAiEAvh1tMC31J%2BXuUAgdB5qZ9pvwEYWTRs%2BvL70j6ejTpxAq%2FAQI7%2F%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDEOuPuQ0pUkByEW%2BjyrQBB%2FJmgrsrFsYh8GPebrDaEf5SZv1%2F3lBBKlVaPUAl7IVHEW37cNf1QwKPwDcSohIYiwFKv5VSP0xwK6zWQ8Cvs2mHE8DxF73mV9ReHaMX45TPtatFcJWLyAoA7OxWbthzsJAE2XyH9lJQ2EIHtJTo16eOR30CrZVm303NromwrU%2BapIaC9Cn%2FQIkyoMPR2A0ym6If0glAhF%2FBWd1TqpQpKzhsBSEOSIIonjcnxG4LAnjMD%2BnKxTOAakotREgX2kKT15LqlX0DjciC9mSofBIncoCprH%2BF3fR%2FFYgmo4QyYpev9jt6NcN7JBIud7iE2pEm4p8fq2nICil5Jwfw7ECo%2F2QVq5A%2FR2LbZ3hYrpJu9828zGxQneiyJEifyfwF2mVqoQJPF26hBYq%2FWeapqsEdqEpozUPRLUEwNZKqLDMIeRXPAT96LFoEM341YLsv3S44fAdDdn8zNYy0qBaVkUyU9oIiCECj8JnJq6f43TW9bI%2BjxBG9Rz303Ko6mR39kOnmBog2efoj1bhyfYCB6RNte9uawolSKX4yAtmkopc4%2FRcXms2vMtMga9cObJ9btXA2tvDYJ2abo1CyDaeJQSIKOrAdhtOSxyYyo5O2shlnNSIdrTGfe2MUkCk1xIj2U3h6QX0h48dWVl2yBqsrrluFfa3mpwGrK5%2F5A37fgOLJrXriXK3iVBNQuz7IQKJWKk5IuBIXh0lewF7d8IgB4wIBSX%2FVtaHeT9IzFQzD7Nz8CKo50VRyIgBJ1UI00%2FA3VpPw0JqZZbw369Rr1v%2Fx%2Bays9cw7LjlzQY6mAF1VSJ11Se7CMLZXCdLqgcuwjDyUN4MDH%2BSkJFv2wpNg%2FVkZCBFs6IRWP6I1tqRZrFUaFydWuY65BXvebbSHbADnGWePlLkTxmRWI%2F7uZB5XzBhm87ETW6CLAqlA8AsV5PDXe9qIux%2FE%2FDMc%2BJun8yuVfIc9ddoOw1C%2FeFW8ElrcMu9PX9TTA6pn5rFyrNhXS59qxLST7Khfg%3D%3D&Expires=1773759039) - # F‑02. Создание одноактивной потоковой заявки (Flow Order)
## 1. Общая информация
### 1.1. Понятия ...

2. [F-04.-Batch-Clearing.md](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/38fc0c4a-9154-4573-b9d2-9503b56309d1/F-04.-Batch-Clearing.md?AWSAccessKeyId=ASIA2F3EMEYE4AG2GW4Y&Signature=HqQ3%2FwexfI3hqZMsYJJyxqdttR4%3D&x-amz-security-token=IQoJb3JpZ2luX2VjECYaCXVzLWVhc3QtMSJHMEUCIG11jVjjGAFhuGl%2FKxcMIErQiuwy0y6XQXwiI%2BKzVpQhAiEAvh1tMC31J%2BXuUAgdB5qZ9pvwEYWTRs%2BvL70j6ejTpxAq%2FAQI7%2F%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDEOuPuQ0pUkByEW%2BjyrQBB%2FJmgrsrFsYh8GPebrDaEf5SZv1%2F3lBBKlVaPUAl7IVHEW37cNf1QwKPwDcSohIYiwFKv5VSP0xwK6zWQ8Cvs2mHE8DxF73mV9ReHaMX45TPtatFcJWLyAoA7OxWbthzsJAE2XyH9lJQ2EIHtJTo16eOR30CrZVm303NromwrU%2BapIaC9Cn%2FQIkyoMPR2A0ym6If0glAhF%2FBWd1TqpQpKzhsBSEOSIIonjcnxG4LAnjMD%2BnKxTOAakotREgX2kKT15LqlX0DjciC9mSofBIncoCprH%2BF3fR%2FFYgmo4QyYpev9jt6NcN7JBIud7iE2pEm4p8fq2nICil5Jwfw7ECo%2F2QVq5A%2FR2LbZ3hYrpJu9828zGxQneiyJEifyfwF2mVqoQJPF26hBYq%2FWeapqsEdqEpozUPRLUEwNZKqLDMIeRXPAT96LFoEM341YLsv3S44fAdDdn8zNYy0qBaVkUyU9oIiCECj8JnJq6f43TW9bI%2BjxBG9Rz303Ko6mR39kOnmBog2efoj1bhyfYCB6RNte9uawolSKX4yAtmkopc4%2FRcXms2vMtMga9cObJ9btXA2tvDYJ2abo1CyDaeJQSIKOrAdhtOSxyYyo5O2shlnNSIdrTGfe2MUkCk1xIj2U3h6QX0h48dWVl2yBqsrrluFfa3mpwGrK5%2F5A37fgOLJrXriXK3iVBNQuz7IQKJWKk5IuBIXh0lewF7d8IgB4wIBSX%2FVtaHeT9IzFQzD7Nz8CKo50VRyIgBJ1UI00%2FA3VpPw0JqZZbw369Rr1v%2Fx%2Bays9cw7LjlzQY6mAF1VSJ11Se7CMLZXCdLqgcuwjDyUN4MDH%2BSkJFv2wpNg%2FVkZCBFs6IRWP6I1tqRZrFUaFydWuY65BXvebbSHbADnGWePlLkTxmRWI%2F7uZB5XzBhm87ETW6CLAqlA8AsV5PDXe9qIux%2FE%2FDMc%2BJun8yuVfIc9ddoOw1C%2FeFW8ElrcMu9PX9TTA6pn5rFyrNhXS59qxLST7Khfg%3D%3D&Expires=1773759039) - ## 1. Общая информация

### 1.1. Понятия и определения  
#### Базовые сущности батч‑клиринга

- **Ba...

