[TOC]
# F‑11. Подключение внешних площадок (CEX/DEX)
## 1. Общая информация
### 1.1. Понятия и определения
#### Базовые сущности внешних площадок

- **Venue (площадка)**  
  Внешняя биржа или пул ликвидности (CEX, DEX, AMM), с которой система непрерывной биржи получает рыночные данные, внешнюю ликвидность и возможность маршрутизировать часть исполнения/хеджа.

- **External Venues Connector**  
  Низкоуровневый компонент подключения к площадкам. Отвечает за WebSocket/REST/RPC‑соединения, heartbeat, reconnect, rate limits, отправку/получение сообщений внешней площадки. Не нормализует данные и не принимает бизнес‑решений.

- **Venue Market Data Normalizer**  
  Компонент нормализации сырых внешних данных в единый внутренний формат `VenueSnapshot`: стакан, сделки, fees, tick/lot size, статусы инструмента, временные метки.

- **VenueSnapshot**  
  Нормализованный снимок внешнего рынка, приведённый к единому формату биржи. Содержит:
  - `snapshotid` — уникальный идентификатор снимка (UUID);
  - `venueid` — идентификатор площадки (`binance`, `coinbase`, `uniswap_v3` и т.д.);
  - `symbol` — торговый инструмент;
  - `timestamp` — UTC‑время события/снимка;
  - `midprice` — средняя цена;
  - `bestbid`, `bestask` — лучшие цены покупки/продажи;
  - `spread` — спред;
  - `volume24h` — объём торгов за 24 часа (если доступен);
  - `biddepth`, `askdepth` — дискретная глубина стакана на стороне bid/ask;
  - `fees` — комиссии площадки;
  - `ticksize`, `lotsize` — минимальные шаги цены и объёма;
  - `status` — `connected | stale | disconnected | empty`.

- **Venue Liquidity Curve Builder**  
  Компонент, который принимает `VenueSnapshot` и решает задачу перевода дискретного внешнего LOB/AMM в непрерывное FOB‑представление ликвидности. Именно он концентрирует всю сложную логику LOB→FOB и публикует её для downstream‑сервисов.

- **VenueLiquidityCurve**  
  Непрерывное FOB‑представление внешней ликвидности по конкретной площадке и инструменту. Включает, как минимум:
  - сетку по объёму $q$;
  - предельную цену $p(q)$;
  - интегральную стоимость исполнения $S(q)$;
  - представление по скорости $v = q / \tau$;
  - FOB‑лагранжиан $L(v)$;
  - метрики качества аппроксимации и confidence.

- **SyntheticFlowOrder**  
  Виртуальная FlowOrder, сгенерированная из внешней ликвидности и предназначенная для совместимости с FOB‑ядром клиринга. Участвует в батч‑клиринге наряду с реальными клиентскими FlowOrder и обычно маркируется `liquiditysource = cexhedge | dexhedge`. В текущей архитектуре является производным представлением от `VenueLiquidityCurve`, а не единственной формой представления внешней ликвидности.

- **Venue Health & Routing Service**  
  Компонент оценки качества и здоровья площадок. На основе latency, reconnect, error rate, качества цены и статистики fill rate рассчитывает health‑score и рекомендации по routing/disable площадок.

- **Venue Execution Adapter**  
  Компонент исполнения внешних child‑ордеров/хеджа. Принимает `ExecutionIntent`, маппит внутренний инструмент на venue‑символ, отправляет ордера через External Venues Connector и публикует execution reports.

- **Execution Planning & Forecast**  
  Онлайн‑компонент планирования исполнения. Использует `VenueLiquidityCurve`, health‑score площадок, риск‑лимиты и фактический инвентори для оценки VWAP/IS и построения траектории исполнения.

- **LOB (Limit Order Book)**  
  Дискретный стакан заявок: набор уровней цена–объём на стороне bid и ask.

- **FOB (Flow Order Book)**  
  Представление ликвидности и предпочтений в непрерывной форме, ориентированной на поток исполнения/скорость. FOB‑ядро работает не с сырым LOB, а с кривыми внешней ликвидности и/или синтетическими потоковыми ордерами.

***

#### Формулы нормализации

$$
\text{mid} = \frac{\text{bestBid} + \text{bestAsk}}{2}
$$

$$
\text{spread} = \text{bestAsk} - \text{bestBid}
$$

***

#### LOB‑to‑FOB перевод

Обычный стакан LOB — это набор ступенчатых уровней «по такой цене доступен такой объём». Для непрерывной биржи этого недостаточно: внешний рынок должен быть приведён к непрерывной форме, совместимой с FOB‑ядром, Execution Planning и Risk.

Базовая цепочка преобразования:

1. Из дискретных уровней стакана строится функция предельной цены исполнения $p(q)$ как функция кумулятивного объёма $q$.
2. По ней строится интегральная стоимость исполнения:

$$
S(q) = \int_0^q p(x)\,dx
$$

3. При необходимости $S(q)$ делается выпуклой и гладкой с помощью регуляризации/сглаживания.
4. Через временной масштаб $\tau$ строится скорость исполнения:

$$
v = \frac{q}{\tau}
$$

5. Строится FOB‑представление в виде функции стоимости потока:

$$
L(v) = \frac{S(v\tau)}{\tau}
$$

или, если используется excess‑cost относительно reference price $p_{ref}$:

$$
L(v) = \frac{S(v\tau) - p_{ref} \cdot v\tau}{\tau}
$$

6. Результат публикуется как `VenueLiquidityCurve`; при необходимости дополнительно строится `SyntheticFlowOrder`.

Для инженерной и исследовательской реализации поддерживаются несколько режимов LOB→FOB:
- **Level 1 (Fast)** — быстрая монотонная инженерная аппроксимация;
- **Level 2 (Regularized)** — выпуклая и сглаженная кривая с регуляризацией;
- **Level 3 (Calibrated)** — кривая, дополнительно откалиброванная по фактическим исполнениям и impact‑модели.

***

#### Транспорт и хранилища

- **Kafka `venue.snapshots`**  
  Топик нормализованных `VenueSnapshot`. Потребители: Venue Liquidity Curve Builder, Market Data Service, Backtest & Replay Engine, Observability.

- **Kafka `venue.liquidity.fob`**  
  Топик непрерывных `VenueLiquidityCurve`. Потребители: Matching Backend, Risk Manager, Execution Planning & Forecast, Backtest & Replay Engine.

- **Kafka `venue.synthetic`**  
  Топик `SyntheticFlowOrder` для режима совместимости/упрощённой интеграции с FOB‑ядром.

- **Kafka `venue.health`**  
  Топик heartbeat, статусов соединений и health‑score площадок. Потребители: Risk Manager, Execution Planning & Forecast, Observability.

- **Kafka `execution.venue`**  
  Топик execution reports по внешним child‑ордерам/хеджу. Потребители: Settlement & Ledger, Risk Manager, Observability.

- **venue_snapshots (таблица ClickHouse)**  
  Историческое аналитическое хранилище всех `VenueSnapshot` для backtest, реплея, отчётности и диагностики.

| Поле | Тип | Описание |
|------|-----|----------|
| `snapshotid` | UUID | Уникальный ID снимка |
| `venueid` | String | Идентификатор площадки |
| `symbol` | String | Торговый инструмент |
| `timestamp` | DateTime64 | UTC‑время события |
| `midprice` | Float64 | mid‑цена |
| `bestbid` | Float64 | Лучшая цена покупки |
| `bestask` | Float64 | Лучшая цена продажи |
| `spread` | Float64 | Спред |
| `volume24h` | Float64 | Объём 24ч |
| `biddepth` | String (JSON) | Глубина bid |
| `askdepth` | String (JSON) | Глубина ask |
| `fees` | String (JSON) | Комиссии |
| `ticksize` | Float64 | Шаг цены |
| `lotsize` | Float64 | Шаг объёма |
| `status` | Enum | Статус соединения |

- **venue_liquidity_curves (таблица ClickHouse / PostgreSQL)**  
  Исторические и/или актуальные FOB‑кривые внешней ликвидности.

| Поле | Тип | Описание |
|------|-----|----------|
| `curveid` | UUID | Уникальный ID кривой |
| `venueid` | String | Площадка |
| `symbol` | String | Инструмент |
| `side` | Enum | Сторона `buy/sell` |
| `level` | Enum | Уровень модели `L1/L2/L3` |
| `tau_sec` | Float64 | Временной масштаб |
| `q_grid` | String (JSON) | Сетка объёмов |
| `p_of_q` | String (JSON) | Предельная цена |
| `s_of_q` | String (JSON) | Интегральная стоимость |
| `l_of_v` | String (JSON) | FOB‑кривая по скорости |
| `epsilon1` | Float64 | Ошибка стоимости |
| `epsilon2` | Float64 | Ошибка цены/монотонности |
| `epsilon3` | Float64 | Ошибка исполнения |
| `confidence` | Float64 | Доверие к кривой |
| `createdat` | DateTime64 | Время публикации |

- **venue_config (таблица PostgreSQL)**  
  Конфигурация подключений к внешним площадкам и моделей LOB→FOB.

| Поле | Тип | Описание |
|------|-----|----------|
| `venueid` | VARCHAR(32) PK | Идентификатор площадки |
| `venuetype` | ENUM(`cex`, `dex`, `amm`) | Тип площадки |
| `displayname` | VARCHAR(128) | Отображаемое имя |
| `apiurl` | TEXT | URL API |
| `wsurl` | TEXT | URL WebSocket |
| `symbols` | JSONB | Список поддерживаемых символов |
| `pollingintervalms` | INT | Интервал опроса |
| `reconnectattempts` | INT | Число попыток переподключения |
| `reconnectdelayms` | INT | Задержка между попытками |
| `fees` | JSONB | Комиссии |
| `ticksize` | JSONB | Шаг цены |
| `lotsize` | JSONB | Шаг объёма |
| `lobtofobmodel` | JSONB | Конфигурация модели LOB→FOB |
| `isactive` | BOOLEAN | Флаг активности |
| `createdat` | TIMESTAMPTZ | Дата создания |
| `updatedat` | TIMESTAMPTZ | Дата обновления |

- **synthetic_orders (таблица PostgreSQL)**  
  Синтетические FlowOrder, полученные из `VenueLiquidityCurve` при необходимости совместимости с Matching Backend.

| Поле          | Тип                               | Описание               |
| ------------- | --------------------------------- | ---------------------- |
| `syntheticid` | UUID PK                           | Уникальный ID          |
| `venueid`     | VARCHAR(32) FK                    | Площадка               |
| `symbol`      | VARCHAR(32)                       | Инструмент             |
| `side`        | ENUM(`buy`, `sell`)               | Сторона                |
| `pl`          | NUMERIC(24,8)                     | Нижняя граница цены    |
| `ph`          | NUMERIC(24,8)                     | Верхняя граница цены   |
| `qrate`       | NUMERIC(24,8)                     | Скорость исполнения    |
| `qmax`        | NUMERIC(24,8)                     | Максимальный объём     |
| `curveid`     | UUID FK                           | Исходная FOB‑кривая    |
| `snapshotid`  | UUID FK                           | Исходный VenueSnapshot |
| `createdat`   | TIMESTAMPTZ                       | Время создания         |
| `expiresat`   | TIMESTAMPTZ                       | Время истечения        |
| `status`      | ENUM(`active`, `expired`, `used`) | Статус                 |

***

#### Прочие термины

- **Stale данные**  
  Данные, для которых возраст снимка превысил допустимый порог свежести `stalethresholdms`. Такие данные не должны использоваться для построения новых FOB‑кривых или генерации новых SyntheticFlowOrder.

- **Circuit breaker**  
  Предохранитель на уровне venue/adaptor, который переводит площадку в режим временной блокировки после серии ошибок. После cooldown‑периода выполняется пробное восстановление.

- **Tick size / Lot size**  
  Минимальные шаги цены и объёма. Используются в нормализации, построении кривых и внешнем хедже.

- **Health‑score**  
  Агрегированный показатель качества площадки, учитывающий latency, error rate, fill rate, отклонение цен и stale‑rate.

#### Таблица соответствия сущностей постановки

Вот таблица с добавленной колонкой.

| Компонент системы | Сущность из постановки | Описание сущности из постановки | Отношение сущности к компоненту |
|------------------|------------------------|---------------------------------|---------------------------------|
| External Venues Connector | Venue | Внешняя торговая площадка (CEX/DEX/AMM), на которой биржа исполняет хедж‑ордера и откуда получает рыночные данные. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md) | Подключает площадку, держит сетевое соединение, получает/отправляет сырые сообщения. |
| External Venues Connector | rawSnapshot / rawTrade / rawExecution | Сырые сообщения внешнего API: книги заявок, трейды и отчёты об исполнении в нативном формате каждой площадки. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md) | Сырой внешний поток до нормализации. |
| Venue Market Data Normalizer | VenueSnapshot | Нормализованный снимок рынка по конкретному venue: best bid/ask, mid, спред, глубина, объёмы и служебные поля. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md) | Строит унифицированный снимок рынка из сырых данных. |
| Venue Liquidity Curve Builder | LOB→FOB модель | Модель, переводящая дискретный LOB/AMM‑квоты venue в непрерывную FOB‑кривую «цена–объём» с учётом комиссий и ограничений. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md) | Решает задачу перевода LOB/AMM в FOB‑кривые. |
| Venue Liquidity Curve Builder | VenueLiquidityCurve | Нормализованная непрерывная кривая внешней ликвидности по инструменту и venue, пригодная для FOB‑батч‑клиринга. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md) | Основной выходной объект непрерывной внешней ликвидности. |
| Venue Liquidity Curve Builder | SyntheticFlowOrder | Виртуальная flow‑заявка, эквивалентная внешней ликвидности в формате FOB ядра (совместимость с чистым FOB‑движком). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md) | Производный выход для режима совместимости с FOB‑ядром. |
| Venue Health & Routing Service | venue.health | Агрегированное состояние площадки: up/down, latency, error‑rate, quality‑score и рекомендации по использованию. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md) | Публикует status/health‑score/recommendations. |
| Market Data Service | VenueSnapshot, VenueLiquidityCurve | MarketData‑объекты: нормализованные снапшоты и FOB‑кривые, используемые для расчёта mid/reference цен и метрик. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md) | Использует их для reference prices и аналитики. |
| Matching Backend (FOB Core) | VenueLiquidityCurve / SyntheticFlowOrder | Представление внешней ликвидности в терминах FOB (кривые/flow‑ордера), пригодное для совместного клиринга с клиентскими заявками. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md) | Использует внешнюю ликвидность в FOB‑форме в батч‑клиринге. |
| Risk Manager | venue.health, VenueLiquidityCurve | Данные о доступности venues и стоимости/impact внешней ликвидности (косвенно задают риск исполнения хеджа). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md) | Учитывает доступность venues и стоимость/impact внешней ликвидности. |
| Execution Planning & Forecast | VenueLiquidityCurve, venue.health | Входы для планировщика исполнения: форма кривой ликвидности и здоровье venue, необходимые для расчёта маршрутизации и профиля скорости. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md) | Планирует траекторию исполнения и routing. |
| Venue Execution Adapter | ExecutionIntent / execution.venue | Команда на внешнее исполнение с выбранным venue, инструментом, стороной, целью по объёму/ноционалу и параметрами срочности. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md) | Исполняет внешний хедж и публикует отчёты. |
| Settlement & Ledger | execution.venue / SyntheticFlowOrder | Факты внешних исполнений и соответствующие synthetic‑ордера, которые нужно отразить в позициях и venue‑балансах. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md) | Учитывает внешние исполнения и venue‑балансы. |
| Backtest & Replay Engine | venue_snapshots / venue_liquidity_curves | Исторические VenueSnapshot и VenueLiquidityCurve, восстановленные из ClickHouse/Kafka для оффлайн‑реплея и калибровки. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md) | Использует историю снапшотов и FOB‑кривых для реплея и калибровки. |
| Observability & Reporting | venue.health / venue.snapshots / execution.venue | Набор телеметрии по внешнему контуру: здоровье venues, рыночные снапшоты и фактические исполнения хеджа. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md) | Строит дашборды и алерты по внешнему контуру. |
| Web UI / Trading Frontend | VenueSnapshot / venue.health | Объекты, которые визуализируются оператору: рыночная картина по venue и статусы подключений/качества. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md) | Показывает оператору статусы подключений и рыночную картину. |
| Admin UI | venue_config / venue.health / VenueSnapshot | Конфигурация venue (лимиты, ключи, режимы), состояние площадки и её рыночные снапшоты для администрирования. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md) | Управляет конфигурацией площадок и наблюдает их состояние. |
| API Gateway (Provider–Client) | REST API `/api/v1/venues*` | Внешний HTTP‑интерфейс для запросов списка venues, их статусов и административных операций. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md) | Проксирует административные и операторские вызовы. |
| Message Broker (Apache Kafka) | `venue.snapshots`, `venue.liquidity.fob`, `venue.synthetic`, `venue.health`, `execution.venue` | Топики событий внешнего контура: нормализованные снапшоты, FOB‑кривые, synthetic‑ордера, health и отчёты об исполнении. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md) | Шина событий между компонентами. |
| Event / Analytics DB (ClickHouse) | `venue_snapshots`, `venue_liquidity_curves` | Колонки и таблицы для долговременного хранения снапшотов рынка и рассчитанных FOB‑кривых по venues. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md) | Историческое хранилище рыночных и модельных данных. |
| OLTP PostgreSQL | `venue_config`, `synthetic_orders` | Транзакционные таблицы конфигураций venues и оперативных synthetic‑заявок/состояния для online‑режима. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md) | Транзакционное хранилище конфигураций и оперативного состояния. |

Нужно ли эту же таблицу сразу привести в том формате, как вы используете в Confluence (например, с колонкой FID/F‑XX)?

***
### 1.2. Краткое описание
Фича отвечает за подключение к внешним площадкам (CEX, DEX, AMM), нормализацию их данных в `VenueSnapshot`, построение непрерывных FOB‑кривых внешней ликвидности (`VenueLiquidityCurve`), расчёт mid/spread/reference prices, а также, при необходимости, генерацию `SyntheticFlowOrder` для участия во внутреннем батч‑клиринге. Фича дополнительно включает мониторинг качества площадок (`venue.health`) и использование внешней ликвидности в Execution Planning, Risk и внешнем execution hedge.

### 1.3. Область применения
- External Venues Connector;
- Venue Market Data Normalizer;
- Venue Liquidity Curve Builder;
- Venue Health & Routing Service;
- Venue Execution Adapter;
- Market Data Service;
- Matching Backend;
- Risk Manager;
- Execution Planning & Forecast;
- Settlement & Ledger;
- Backtest & Replay Engine;
- Observability & Reporting;
- Admin UI / API Gateway.

### 1.4. Заинтересованные стороны
- Трейдеры/клиенты — получают лучшие цены, дополнительную внешнюю ликвидность и более предсказуемое исполнение;
- Маркет‑мейкеры — используют внешний рынок как источник benchmark и hedge‑контур;
- Операторы/риск‑офицеры — контролируют статусы площадок, качество ликвидности, стоимость исполнения и деградации;
- Команды Matching, Risk, Market Data, Execution Planning, Venue Execution, Observability, Backtest.

***
## 2. Бизнес‑часть (BRD)
### 2.1. Назначение и цели
Функция предназначена для:
- получения рыночных данных и глубины рынка с внешних площадок как источника reference prices;
- построения источника внешней ликвидности в FOB‑форме для внутреннего клиринга, оценки риска и планирования исполнения;
- обеспечения внешнего execution hedge после внутреннего клиринга;
- публикации операторских статусов площадок и качества их работы.

Цели:
- обеспечить покрытие не менее 3 внешних площадок для MVP (например, 2 CEX + 1 DEX/AMM);
- обеспечить свежесть нормализованных данных: p95 задержка получения `VenueSnapshot` < 500 ms;
- обеспечить p95 построения FOB‑кривой < 50 ms на один инструмент в режиме L1/L2;
- обеспечить контролируемую точность LOB→FOB:
  - ошибка стоимости $\epsilon_1$ не более 1% на рабочем диапазоне объёмов;
  - отсутствие нарушения монотонности/выпуклости после регуляризации;
  - контролируемую ошибку предсказания исполнения $\epsilon_3$ для калиброванных моделей L3.

### 2.2. Предусловия
- Система авторизации настроена, права операторов и сервисные креденшелы площадок заведены в secure vault;
- Конфигурация площадок (`venue_config`) задана и активна;
- Kafka‑топики `venue.snapshots`, `venue.liquidity.fob`, `venue.synthetic`, `venue.health`, `execution.venue` созданы;
- Market Data Service готов потреблять `VenueSnapshot` и FOB‑кривые;
- Matching Backend готов принимать внешнюю ликвидность в форме `VenueLiquidityCurve` и/или `SyntheticFlowOrder`;
- Risk Manager и Execution Planning готовы использовать `venue.health` и FOB‑кривые;
- ClickHouse / Event DB готовы принимать снапшоты и кривые для истории.

### 2.3. Основной успешный сценарий
#### 2.3.1. Диаграмма последовательности (бизнес‑уровень)

Участники:
- Оператор;
- Система непрерывной биржи;
- Внешняя площадка CEX/DEX/AMM.

Сценарий:
1. Оператор добавляет/активирует площадку в `venue_config`.
2. External Venues Connector подключается к площадке и начинает получать сырые данные рынка.
3. Venue Market Data Normalizer строит `VenueSnapshot`.
4. Venue Liquidity Curve Builder строит `VenueLiquidityCurve` и, при необходимости, `SyntheticFlowOrder`.
5. Market Data Service обновляет reference prices.
6. Matching Backend, Risk и Execution Planning используют FOB‑представление внешней ликвидности.
7. При необходимости система формирует ExecutionIntent и отправляет child‑ордера через Venue Execution Adapter.
8. Оператор видит в UI состояние площадки, качество ликвидности, health‑score и влияние на клиринг.

```mermaid
sequenceDiagram
    participant O as Оператор
    participant S as Система непрерывной биржи
    participant V as Внешняя площадка CEX/DEX/AMM

    O->>S: Добавить/активировать площадку в venue_config
    S-->>O: Подтверждение активации

    loop Постоянно
        S->>V: Подписка/опрос рыночных данных
        V-->>S: Сырые стаканы, сделки, статусы
        S->>S: Нормализация в VenueSnapshot
        S->>S: Построение VenueLiquidityCurve (LOB→FOB)
        S->>S: Обновление reference prices и внешней ликвидности
        S->>S: Использование кривой в Matching / Risk / ExecPlan
    end

    S-->>O: Статусы площадок, качество ликвидности, метрики
```

#### 2.3.2. Описание действий

1. Оператор добавляет новую площадку в `venue_config` через Admin UI или Admin API.
2. External Venues Connector обнаруживает изменение конфигурации и инициализирует соответствующий venue‑adapter.
3. Adapter устанавливает WebSocket‑соединение (CEX) или RPC/subscription (DEX/AMM), получает сырые данные рынка.
4. Venue Market Data Normalizer приводит сырые данные к единому `VenueSnapshot`.
5. Venue Liquidity Curve Builder принимает `VenueSnapshot`, строит непрерывную FOB‑кривую внешней ликвидности и публикует её в `venue.liquidity.fob`.
6. Для режима совместимости дополнительно создаётся `SyntheticFlowOrder`, который публикуется в `venue.synthetic` и сохраняется в `synthetic_orders`.
7. Market Data Service использует `VenueSnapshot` и FOB‑кривые для расчёта mid/spread/reference prices.
8. Matching Backend использует внешнюю ликвидность в FOB‑форме в составе общего пула ликвидности.
9. Execution Planning & Forecast использует те же кривые для расчёта ожидаемого VWAP/IS и маршрута исполнения.
10. При необходимости хеджа формируется `ExecutionIntent`, который исполняется через Venue Execution Adapter и External Venues Connector.
11. Settlement & Ledger получает execution reports, обновляет venue‑балансы, позиции и PnL.

### 2.4. Дополнительные сценарии
**Сценарий: Потеря соединения с площадкой**
1. External Venues Connector обнаруживает disconnect/timeout.
2. Запускается reconnect‑логика с конфигурируемым числом попыток.
3. Последний `VenueSnapshot` помечается как `stale`.
4. Venue Liquidity Curve Builder прекращает публикацию новых активных FOB‑кривых по этой площадке.
5. `venue.health` публикует деградацию площадки.
6. Risk/ExecPlan перестают использовать площадку или понижают её приоритет.

**Сценарий: Circuit breaker**
1. По площадке фиксируется серия ошибок за окно наблюдения.
2. Площадка переводится в `OPEN` и временно блокируется.
3. После cooldown выполняется пробный запрос (`HALF_OPEN`).
4. При успехе площадка возвращается в `CLOSED`, при неудаче остаётся в `OPEN`.
5. Все переходы публикуются в `venue.health` и отображаются в UI.

**Сценарий: DEX / AMM без классического стакана**
1. Adapter получает состояние пула (например, `sqrtPriceX96`, `tick`, `liquidity`, ticks для Uniswap v3).
2. Venue Market Data Normalizer формирует `VenueSnapshot` для DEX/AMM.
3. Venue Liquidity Curve Builder синтезирует виртуальный LOB/непосредственно FOB‑кривую из состояния пула.
4. Далее процесс идентичен CEX: нормализация → FOB‑кривая → клиринг/риск/планирование.

**Сценарий: Пустая или нулевая ликвидность**
1. Для площадки отсутствует bid/ask или доступная ликвидность в рабочем диапазоне.
2. `VenueSnapshot` получает статус `empty`.
3. FOB‑кривая и `SyntheticFlowOrder` не публикуются.
4. Увеличивается счётчик `venue.empty_snapshots`.

**Сценарий: Несколько площадок для одного инструмента**
1. Подключены несколько venues для одного инструмента.
2. Каждая площадка публикует собственный `VenueSnapshot` и `VenueLiquidityCurve`.
3. Matching / Risk / ExecPlan используют все доступные источники с учётом health‑score.
4. Market Data Service строит агрегированные BBO/reference prices.

```mermaid
sequenceDiagram
    participant C as External Venues Connector
    participant B as Venue Liquidity Curve Builder
    participant H as Venue Health & Routing
    participant R as Risk / ExecPlan

    alt Потеря соединения
        C->>C: timeout / disconnect
        C->>H: venue.health = stale/disconnected
        H->>R: Понизить/отключить площадку
        B->>B: Не публиковать новые FOB-кривые
    else Circuit breaker
        C->>H: серия ошибок
        H->>H: OPEN -> HALF_OPEN -> CLOSED
        H->>R: routing update
    else Пустой рынок
        C->>B: empty VenueSnapshot
        B->>B: skip curve publication
        H->>H: empty snapshot metric++
    end
```

### 2.5. UX / UI
```text
+--------------------------------------------------------------------+
|                Admin UI: Внешние площадки и ликвидность            |
+--------------------------------------+-----------------------------+
| Список площадок                      | Детали площадки             |
|                                      |                             |
| - Таблица venue_config               | - Текущий VenueSnapshot     |
|   * venueId                          |   * midPrice                |
|   * displayName                      |   * bestBid / bestAsk       |
|   * type (CEX/DEX/AMM)               |   * spread                  |
|   * status                           |   * bidDepth / askDepth     |
|   * healthScore                      |   * fees / tick / lot       |
|   * lastSnapshotAge                  |                             |
|                                      | - FOB-кривая ликвидности    |
| Действия:                            |   * level L1/L2/L3          |
| - Добавить площадку                  |   * confidence              |
| - Активировать / деактивировать      |   * epsilon1/2/3            |
| - Редактировать config               |   * q-grid / p(q) preview   |
| - Force reconnect                    |                             |
|                                      | - SyntheticFlowOrder        |
|                                      |   * pL, pH, qRate, qMax     |
|                                      |   * status                  |
|                                      |                             |
|                                      | - Метрики                   |
|                                      |   * latency p50/p95         |
|                                      |   * error rate              |
|                                      |   * snapshots/sec           |
|                                      |   * stale rate              |
|                                      |   * circuit breaker state   |
+--------------------------------------+-----------------------------+
```

### 2.6. Критерии успеха
- Для каждой активной площадки получаются и нормализуются `VenueSnapshot` с p95 задержкой < 500 ms;
- Venue Liquidity Curve Builder публикует валидные FOB‑кривые по каждой активной площадке;
- В режиме совместимости генерируются корректные `SyntheticFlowOrder`;
- Ошибка стоимости LOB→FOB на рабочем диапазоне не превышает 1%;
- При потере соединения площадка уходит в degraded mode без остановки внутреннего клиринга;
- Risk и Execution Planning корректно учитывают `venue.health` и отключают/понижают деградировавшие venues;
- Dashboard оператора отображает статусы площадок, health‑score, качество FOB‑кривых и execution quality.

***
## 3. Требования
### 3.1. Функциональные требования
F11‑1. Система должна поддерживать подключение к CEX по WebSocket и/или REST API.

F11‑2. Система должна поддерживать подключение к DEX/AMM через RPC/event subscription и/или индексирующие сервисы.

F11‑3. Для каждого полученного внешнего рыночного события система должна строить `VenueSnapshot` с полями `bestBid`, `bestAsk`, `midPrice`, `spread`, `fees`, `tickSize`, `lotSize`, `status`.

F11‑4. Система должна публиковать `VenueSnapshot` в Kafka‑топик `venue.snapshots` и сохранять историю снапшотов в ClickHouse.

F11‑5. Система должна иметь отдельный компонент Venue Liquidity Curve Builder, который принимает `VenueSnapshot` и строит `VenueLiquidityCurve`.

F11‑6. Venue Liquidity Curve Builder должен поддерживать не менее трёх режимов LOB→FOB: Level 1 (Fast), Level 2 (Regularized), Level 3 (Calibrated).

F11‑7. В режиме Level 1 система должна строить монотонную аппроксимацию $p(q)$, интегральную стоимость $S(q)$ и FOB‑форму $L(v)$.

F11‑8. В режиме Level 2 система должна поддерживать:
- приведение кривой к выпуклой форме;
- регуляризацию Moreau и/или Tikhonov;
- публикацию метрик качества $\epsilon_1$, $\epsilon_2$.

F11‑9. В режиме Level 3 система должна поддерживать калибровку impact‑модели по фактическим execution reports и публикацию ошибки исполнения $\epsilon_3$.

F11‑10. Система должна публиковать `VenueLiquidityCurve` в Kafka `venue.liquidity.fob` для потребления Matching Backend, Risk Manager и Execution Planning & Forecast.

F11‑11. Система должна поддерживать производное построение `SyntheticFlowOrder` из `VenueLiquidityCurve` и публиковать его в Kafka `venue.synthetic`, если включён режим совместимости.

F11‑12. Matching Backend должен использовать внешнюю ликвидность только в FOB‑форме (`VenueLiquidityCurve` и/или `SyntheticFlowOrder`) и не работать напрямую с сырым LOB.

F11‑13. Risk Manager должен учитывать `venue.health` и ограничения по доступности/качеству площадок при расчёте решений ok/reject/throttle.

F11‑14. Execution Planning & Forecast должен использовать `VenueLiquidityCurve` и `venue.health` для расчёта планов исполнения и routing.

F11‑15. Venue Execution Adapter должен принимать `ExecutionIntent`, маппить внутренние инструменты на venue‑символы, отправлять child‑ордера через External Venues Connector и публиковать `execution.venue`.

F11‑16. Система должна реализовать stale detection: при превышении `stalethresholdms` новые FOB‑кривые и `SyntheticFlowOrder` по площадке не строятся.

F11‑17. Система должна реализовать circuit breaker для каждой площадки/adapter с конфигурируемыми порогами и cooldown‑периодом.

F11‑18. Для DEX/AMM без классического стакана система должна синтезировать виртуальный LOB или непосредственно FOB‑кривую из состояния пула ликвидности.

F11‑19. Admin API должен предоставлять CRUD для `venue_config`, включая параметры подключения, модель LOB→FOB и hot reload без перезапуска сервиса.

F11‑20. Система должна логировать и хранить версии моделей LOB→FOB, параметры их калибровки и quality‑метрики для последующего backtest/replay.

### 3.2. Нефункциональные требования
- Задержка получения и нормализации `VenueSnapshot`: p95 < 500 ms от момента публикации данных площадкой;
- Пропускная способность: не менее 100 `VenueSnapshot`/sec суммарно по всем площадкам для MVP;
- Построение FOB‑кривой L1/L2: p95 < 50 ms на один инструмент;
- Публикация обновления `venue.health`: не позднее 1 сек после существенного инцидента соединения;
- Ошибка стоимости LOB→FOB $\epsilon_1$: не более 1% на рабочем диапазоне объёмов;
- Отсутствие нарушения монотонности/выпуклости после регуляризации для L2;
- История `VenueSnapshot` и `VenueLiquidityCurve` хранится в ClickHouse не менее 90 дней;
- Изменения `venue_config` и модели LOB→FOB применяются hot reload;
- При полном отказе всех внешних площадок система продолжает работать на внутренней ликвидности (graceful degradation);
- Должна быть обеспечена деградация уровней моделей L3 → L2 → L1 → OFF при ухудшении качества данных.

***
## 4. Техническая архитектура (TRD)
### 4.1. Состав компонентов
- **External Venues Connector** — низкоуровневые адаптеры CEX/DEX/AMM;
- **Venue Market Data Normalizer** — нормализация внешних LOB/trades/fees в `VenueSnapshot`;
- **Venue Liquidity Curve Builder** — компонент LOB→FOB, публикующий `VenueLiquidityCurve` и опционально `SyntheticFlowOrder`;
- **Venue Health & Routing Service** — health‑score, stale‑control, routing‑recs;
- **Venue Execution Adapter** — внешнее исполнение child‑ордеров/хеджа;
- **PostgreSQL** — `venue_config`, `synthetic_orders`, модельные параметры;
- **ClickHouse / Event DB** — `venue_snapshots`, `venue_liquidity_curves`, execution history;
- **Kafka** — `venue.snapshots`, `venue.liquidity.fob`, `venue.synthetic`, `venue.health`, `execution.venue`;
- **Market Data Service** — reference prices и агрегаты;
- **Matching Backend** — потребитель FOB‑кривых;
- **Risk Manager** — риск‑проверки с учётом качества площадок и внешней ликвидности;
- **Execution Planning & Forecast** — онлайн‑планирование исполнения;
- **Settlement & Ledger** — учёт внешних исполнений и venue‑балансов;
- **Observability & Reporting** — дашборды, алерты, отчёты;
- **Admin UI / API Gateway** — управление площадками и мониторинг.

### 4.2. Диаграмма последовательности (техническая)

```mermaid
sequenceDiagram
    participant Admin as Admin UI
    participant GW as API Gateway
    participant CFG as Admin / VenueConfig Service
    participant EVC as External Venues Connector
    participant VENUE as External Venue (CEX/DEX/AMM)
    participant H as Venue Health & Routing
    participant PG as PostgreSQL venue_config
    participant NORM as Venue Market Data Normalizer
    participant CURVE as Venue Liquidity Curve Builder
    participant K as Kafka (venues & exec)
    participant MDS as Market Data Service
    participant MB as Matching Backend
    participant RISK as Risk Manager
    participant PLAN as Execution Planning
    participant EXEC as Venue Execution Adapter
    participant LEDGER as Settlement & Ledger
    participant CH as ClickHouse

    %% 1. Админ задаёт конфиг и запускает тест подключения
    Admin->>GW: HTTP POST /admin/venue-config/test<br/>(candidate venue_config)
    GW->>CFG: Forward candidate venue_config

    %% 2. CFG просит EVC протестировать подключение с этим конфигом
    CFG-->>EVC: TestConnect(venueId, candidateConfig)

    %% 3. EVC в роли клиента инициирует связь с внешней площадкой (тест)
    EVC->>VENUE: Test connect (HTTP/WebSocket/RPC)
    VENUE-->>EVC: Test handshake / auth OK or error

    EVC->>VENUE: Test request (ping / depth / account)
    VENUE-->>EVC: Test response (depth/trades/status or error)

    %% 4. Оценка health через Venue Health & Routing
    EVC-->>H: Raw test metrics<br/>(latency, errors, status)
    H->>H: Aggregate healthScore & diagnostics
    H-->>CFG: TestResult(venueId, healthScore, diagnostics)

    %% 5. Ответ администратору по результатам теста
    CFG-->>GW: HTTP 200/4xx + TestResult
    GW-->>Admin: Show healthScore / errors / recommendations

    %% 6. При успешном тесте – сохранение конфига и боевой hot-reload
    alt Test OK (healthScore above threshold)
        Admin->>GW: HTTP POST /admin/venue-config/commit<br/>(approved venue_config)
        GW->>CFG: Forward approved venue_config
        CFG->>PG: INSERT/UPDATE venue_config
        PG-->>CFG: Write OK
        CFG-->>EVC: ConfigChanged(venueId, newVersion)

        %% 6.1. EVC загружает финальный конфиг и применяет его
        EVC->>PG: Load venue_config(venueId)
        PG-->>EVC: Full venue_config row
        EVC->>EVC: Apply new config (URLs, keys, timeouts, fees, models)

        %% 7. Постоянное подключение к venue за маркет-данными (боевой режим)
        EVC->>VENUE: Connect & subscribe<br/>(depth / trades / pool state)
        VENUE-->>EVC: Streaming market data<br/>(LOB / trades / pool) [server→client]

        %% 8. Внутренний поток в Normalizer
        EVC-->>NORM: Raw LOB / trades / fees / pool state

        NORM->>K: Publish VenueSnapshot<br/>(topic: venue.snapshots)
        NORM->>K: Publish connection status / raw health<br/>(topic: venue.health)

        %% 9. Построение кривых и синтетических FOB-потоков
        K-->>CURVE: Consume VenueSnapshot<br/>(venue.snapshots)
        CURVE->>CURVE: Build VenueLiquidityCurve (L1/L2/L3)
        CURVE->>K: Publish VenueLiquidityCurve<br/>(topic: venue.liquidity.fob)
        CURVE->>K: Publish SyntheticFlowOrder (optional)<br/>(topic: venue.synthetic)
        CURVE->>CH: Store historical curves

        %% 10. Использование снапшотов и FOB-кривых downstream
        K-->>MDS: Consume snapshots<br/>(venue.snapshots)
        K-->>MDS: Consume FOB curves<br/>(venue.liquidity.fob)
        K-->>MB: External FOB liquidity<br/>(venue.liquidity.fob)
        K-->>RISK: Liquidity/impact inputs<br/>(venue.liquidity.fob)
        K-->>PLAN: Planning inputs<br/>(venue.liquidity.fob)

        %% 11. Health & routing в боевом режиме
        K-->>H: Health aggregation input<br/>(venue.health)
        H->>K: Publish health-score / routing recs<br/>(venue.health)

        %% 12. Исполнение хеджа / внешних ордеров
        PLAN->>EXEC: ExecutionIntent
        EXEC->>EVC: Venue order command
        EVC->>VENUE: Send child orders / cancel
        VENUE-->>EVC: Execution reports / statuses
        EVC-->>EXEC: Raw execution status
        EXEC->>K: Publish execution reports<br/>(topic: execution.venue)

        %% 13. Пост-трейд и учёт
        K-->>LEDGER: External execution reports<br/>(execution.venue)
        K-->>RISK: Post-trade updates<br/>(execution.venue)

        %% 14. Хранение venue-снапшотов
        K-->>CH: Store snapshots<br/>(venue.snapshots)
    else Test FAILED (healthScore too low / error)
        CFG-->>GW: HTTP 400 + TestResult(error, diagnostics)
        GW-->>Admin: Show error, do NOT save venue_config
    end
```

Ниже описание по шагам в том порядке, как на последней диаграмме.

**1. Админ задаёт конфиг и запускает тест**

1. Админ в **Admin UI** заполняет параметры новой площадки (URL, тип CEX/DEX/AMM, ключи/API‑token, fees, tick/lot, модель LOB→FOB и т.п.) и нажимает «Проверить подключение». [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
2. UI отправляет `HTTP POST /admin/venue-config/test` через **API Gateway**, передавая *кандидатный* `venue_config` (ещё не сохранённый в БД). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
3. **API Gateway** после аутентификации и проверки ролей форвардит запрос в **CFG (Admin / VenueConfig Service)** как внутренний вызов с DTO `candidate venue_config`. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**2. CFG просит EVC протестировать подключение**

4. **CFG** не пишет конфиг в БД сразу, а сначала вызывает **External Venues Connector (EVC)** с командой `TestConnect(venueId, candidateConfig)`, передавая все параметры подключения. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**3. EVC как клиент подключается к внешней площадке (тест)**

5. **EVC** в роли **клиента** инициирует исходящее соединение к **VENUE** по указанному в `candidateConfig` протоколу: HTTP/WebSocket/gRPC/JSON‑RPC или on‑chain RPC, в зависимости от типа CEX/DEX/AMM. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
6. **VENUE** (внешняя биржа/AMM) в роли **сервера** принимает соединение и возвращает результат handshake: успех или ошибку аутентификации/авторизации/формата. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
7. При успешном handshake **EVC** отправляет один или несколько тестовых запросов: например, ping, запрос текущего **order book/depth**, последние трейды, `getAccountInfo`/`getBalance` в sandbox и т.п. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
8. **VENUE** отвечает на эти тестовые запросы: возвращает depth/trades/account‑состояние или ошибку (timeout, 4xx/5xx, invalid payload), что позволяет оценить работоспособность конфига. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**4. Расчёт healthScore через Venue Health & Routing**

9. На основе результатов теста (latency, error‑rate, типы ошибок, валидность payload) **EVC** отправляет в **Venue Health & Routing (H)** сырые метрики теста: `latency`, `errorCount`, `status`, возможные коды ошибок и т.п. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
10. **H** агрегирует эти метрики в **healthScore** и дополнительные диагностические поля: причины деградации, рекомендации (например, «latency high», «auth failed», «endpoint unreachable») и классификацию (ok / degraded / failed). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
11. **H** возвращает в **CFG** структуру `TestResult(venueId, healthScore, diagnostics)`, которая говорит, можно ли принимать конфиг, и что именно не так при ошибке. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**5. Ответ администратору**

12. **CFG** формирует HTTP‑ответ, вкладывая в него `TestResult` (успех/ошибка, healthScore, диагностические детали) и отдаёт его в **API Gateway**. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
13. **API Gateway** возвращает ответ в **Admin UI**, который показывает администратору результат проверки: успешное подключение с оценкой качества или подробное описание ошибки и рекомендации по исправлению. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**6. Коммит конфига и боевой hot‑reload (ветка успеха)**

14. Если healthScore выше порога и админ согласен, он нажимает «Сохранить конфигурацию», UI отправляет `HTTP POST /admin/venue-config/commit` через **API Gateway** с уже **одобренным** `venue_config`. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
15. **API Gateway** форвардит этот запрос в **CFG** как `approved venue_config`. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
16. **CFG** записывает конфигурацию в **PostgreSQL `venue_config`**: `INSERT` новой записи или `UPDATE` существующей строки. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
17. **PostgreSQL** подтверждает успешную запись (Write OK), и CFG знает, что теперь БД содержит актуальную версию конфига. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
18. **CFG** отправляет в **EVC** уведомление `ConfigChanged(venueId, newVersion)`, сообщая, что для данного venue есть новая зафиксированная конфигурация, которую нужно подхватить для боевого режима. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**7. EVC читает финальный конфиг и применяет его**

19. Получив `ConfigChanged`, **EVC** обращается к **PostgreSQL `venue_config`**: `Load venue_config(venueId)`, чтобы прочитать *точно тот* конфиг, который зафиксирован как истина. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
20. **PostgreSQL** возвращает полную строку `venue_config` (URL, wsUrl, credentials, fees, tick/lot, модели LOB→FOB, reconnect‑настройки, isActive, и т.п.). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
21. **EVC** применяет новый конфиг к своим адаптерам: обновляет URL/ключи, таймауты, параметры reconnect, модели комиссий и пр., и при необходимости выполняет hot‑reload — закрывает старые коннекты, открывает новые, обновляет подписки. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**8. Постоянное подключение к VENUE и приём маркет‑данных**

22. В боевом режиме **EVC** в роли **клиента** устанавливает (или переустанавливает) соединение к **VENUE** и отправляет команды **subscribe** на depth, trades, pool state и другие нужные каналы. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
23. **VENUE** в роли **сервера** принимает подписку и начинает **стримить маркет‑данные**: потоки LOB, трейдов, состояния пула и сервисные сообщения (heartbeats, partial updates). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
24. **EVC** получает эти сырые потоки (иногда в специфичном для площадки формате), нормализует их на уровне адаптера до внутреннего raw‑формата и передаёт дальше внутрь системы в **Venue Market Data Normalizer (NORM)** как `Raw LOB / trades / fees / pool state`. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**9. Нормализация в VenueSnapshot и health‑события**

25. **NORM** на основе сырых данных формирует нормализованный **VenueSnapshot**: bestBid, bestAsk, midPrice, spread, depth, fees, tick/lot size, статус connected/stale/disconnected/empty, timestamp и т.п. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
26. **NORM** публикует VenueSnapshot в **Kafka** в топик `venue.snapshots`, делая его доступным для Market Data Service, CURVE, backtest и других потребителей. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
27. Параллельно **NORM** формирует и публикует базовые health‑события (connection status, latency, error‑rate и др.) в топик `venue.health`, который является источником для агрегированного health‑routing. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**10. Построение VenueLiquidityCurve и SyntheticFlowOrder**

28. **Venue Liquidity Curve Builder (CURVE)** подписан на `venue.snapshots` и получает VenueSnapshot для разных инструментов и venue. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
29. На их основе **CURVE** рассчитывает **VenueLiquidityCurve** (L1/L2/L3): кривые \(p(q)\), дополнительные расходы \(S(q)\), «скорость» исполнения \(v(q)\), доверительные интервалы, epsilon‑параметры и т.п., по алгоритму LOB→FOB. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
30. **CURVE** публикует VenueLiquidityCurve в Kafka‑топик `venue.liquidity.fob`, откуда её читают Matching Backend, Risk, Execution Planning и Market Data Service. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
31. При необходимости **CURVE** создаёт синтетический **SyntheticFlowOrder** (FlowOrder, представляющий внешнюю FOB‑ликвидность) и отправляет его в Kafka‑топик `venue.synthetic`. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
32. **CURVE** также сохраняет рассчитанные кривые в **ClickHouse** для исторического анализа и бэктеста (таблица `venueliquiditycurves`). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**11. Использование снапшотов и кривых downstream**

33. **Market Data Service (MDS)** подписан на `venue.snapshots` и `venue.liquidity.fob`, формируя BBO, reference prices и агрегированные метрики ликвидности для UI и backend. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
34. **Matching Backend (MB)** использует VenueLiquidityCurve как внешний источник FOB‑ликвидности при решении задач matching/hedging: решает, сколько потока вести внутрь, сколько — на внешние venue. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
35. **Risk Manager (RISK)** берёт VenueLiquidityCurve для оценки impact‑функций и доступной ликвидности по venue, что влияет на лимиты и риск‑политики. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
36. **Execution Planning (PLAN)** использует VenueLiquidityCurve для построения ExecutionIntent: выбор venue, объёмов, тайминга и лимитов цены. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

**12. Health & routing в боевом режиме**

37. **H** подписан на Kafka‑топик `venue.health` и получает сырые и нормализованные health‑события (статус адаптера, stale‑rate, latency, circuit‑breaker‑state и др.). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
38. **H** агрегирует их в текущий healthScore, формирует routing‑рекомендации (какие venue включены/выключены, penalization) и публикует обратно в `venue.health` для Risk, Execution Planning, UI и observability. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

## 13. Исполнение ордеров на внешнем venue

39. **Execution Planning (PLAN)** исходя из кривых ликвидности и healthScore формирует **ExecutionIntent**: venue, symbol, side, target quantity/notional, urgency, лимиты цены и пр. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
40. **PLAN** отправляет ExecutionIntent в **Venue Execution Adapter (EXEC)**, который отвечает за трансляцию в формат конкретного venue. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
41. **EXEC** формирует child‑ордера/команды и отправляет их в **EVC**, который уже общается с внешней площадкой. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
42. **EVC** в роли клиента шлёт соответствующие запросы/ордера на **VENUE** (сервер), используя ранее установленные коннекты и протоколы venue. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
43. **VENUE** возвращает отчёты об исполнении/статусах (NEW, PARTIAL_FILLED, FILLED, CANCELLED, REJECTED, цены, объёмы, fees).  
44. **EVC** передаёт эти сырые execution‑статусы обратно в **EXEC** во внутреннем нормализованном формате. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
45. **EXEC** публикует нормализованные execution reports в Kafka‑топик `execution.venue`. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

## 14. Пост‑трейд и учёт

46. **Settlement & Ledger (LEDGER)** подписан на `execution.venue` и обновляет внешние позиции, PnL по venue, привязку к SyntheticFlowOrder и пр. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)
47. **Risk Manager (RISK)** также подписан на `execution.venue` и использует эти данные для пост‑трейд риска: пересчёт VaR/CVaR, margin, возможные margin‑calls и throttling по venue/клиенту. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

## 15. Хранение снапшотов

48. **ClickHouse (CH)** подписан на `venue.snapshots` и сохраняет VenueSnapshot в таблицу `venuesnapshots`, обеспечивая исторический трек для анализа качества venue, бэктестов и отчётности. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3203bcd9-b9cf-4e68-a6c6-188abe796215/F-11_corrected.md)

Если хотите, могу в следующем шаге оформить компактную таблицу «кто клиент / кто сервер / какие протоколы» для пар EVC↔VENUE, Admin UI↔CFG, EXEC↔EVC.



### 4.3. Техническая интеграция
#### 4.3.1. Интеграции

| Источник | Протокол | Назначение | Описание |
|----------|----------|------------|----------|
| External Venues Connector ↔ CEX | WebSocket / REST | Внешняя площадка | Подписка на depth/trade/status и отправка ордеров |
| External Venues Connector ↔ DEX/AMM | JSON‑RPC / event subscription / indexer API | Внешняя площадка | Получение состояния пула, событий, отправка tx/quote requests |
| External Venues Connector → Venue Market Data Normalizer | Internal stream / Kafka | Сырые данные | Сырые LOB/trades/fees/status |
| Venue Market Data Normalizer → Kafka | Kafka Producer | `venue.snapshots` | Публикация `VenueSnapshot` |
| Venue Liquidity Curve Builder ← Kafka | Kafka Consumer | `venue.snapshots` | Потребление снимков для LOB→FOB |
| Venue Liquidity Curve Builder → Kafka | Kafka Producer | `venue.liquidity.fob` | Публикация FOB‑кривых |
| Venue Liquidity Curve Builder → Kafka | Kafka Producer | `venue.synthetic` | Публикация `SyntheticFlowOrder` |
| Venue Health & Routing → Kafka | Kafka Producer | `venue.health` | Публикация health‑score и статусов |
| Execution Planning → Venue Execution Adapter | Internal RPC / Kafka | ExecutionIntent | Передача плана внешнего исполнения |
| Venue Execution Adapter ↔ External Venues Connector | Internal RPC | Отправка и статусы ордеров | Низкоуровневое исполнение |
| Venue Execution Adapter → Kafka | Kafka Producer | `execution.venue` | Публикация execution reports |
| Market Data Service ← Kafka | Kafka Consumer | `venue.snapshots`, `venue.liquidity.fob` | Reference prices и аналитика |
| Matching Backend ← Kafka / PostgreSQL | Kafka / SQL | FOB‑ликвидность | Внешняя ликвидность для батча |
| Risk Manager ← Kafka | Kafka Consumer | `venue.health`, `venue.liquidity.fob`, `execution.venue` | Риск‑контур |
| ClickHouse ← Kafka | Kafka Consumer | snapshots / curves / execution | Аналитика и history |
| Admin UI → PostgreSQL / API | REST / SQL | `venue_config` | CRUD конфигурации |

#### 4.3.2. Изменяемые объекты конфигурации
- Таблица `venue_config` в PostgreSQL:
  - сетевые настройки площадки;
  - tick/lot/fees;
  - список символов;
  - параметры reconnect/circuit breaker;
  - конфигурация LOB→FOB модели;
- Kafka topics и retention policy;
- Environment variables:
  - `STALE_THRESHOLD_MS`;
  - `CIRCUIT_BREAKER_ERRORS`;
  - `CIRCUIT_BREAKER_WINDOW_S`;
  - `CIRCUIT_BREAKER_COOLDOWN_S`;
  - `LOB_TO_FOB_DEFAULT_LEVEL`;
  - `LOB_TO_FOB_TAU_SEC`.

### 4.4. Способ реализации
#### Алгоритм `NormalizeSnapshot`

```text
Вход: rawMarketData, venueConfig

1. Извлечь сырой стакан / сделки / fee-структуру / статусы инструмента.
2. Если площадка CEX:
   2a. Построить корректный локальный LOB из snapshot + diff updates.
3. Если площадка DEX/AMM:
   3a. Получить состояние пула (tick, liquidity, sqrtPrice и т.д.).
4. Нормализовать уровни bid/ask в единый формат.
5. Если доступный bid/ask отсутствует:
   5a. Вернуть VenueSnapshot со status="empty".
6. Вычислить bestBid, bestAsk.
7. Вычислить mid = (bestBid + bestAsk) / 2.
8. Вычислить spread = bestAsk - bestBid.
9. Добавить fees, tickSize, lotSize, statuses.
10. Сформировать VenueSnapshot.
11. Определить freshness status: connected / stale / disconnected / empty.
12. Вернуть VenueSnapshot.
```

#### Алгоритм `ConvertLOBtoFOB`

```text
Вход: venueSnapshot, modelConfig(level, tau, qMax, regularizationParams, calibrationParams)

1. Извлечь bidLevels и askLevels из venueSnapshot.
2. Построить кумулятивные кривые объёма и предельной цены p(q).
3. Учитывать fees и шаги tick/lot.
4. Если level == L1:
   4a. Построить монотонную инженерную аппроксимацию p(q).
   4b. Интегрировать p(q) -> S(q).
   4c. Построить v = q/tau и L(v) = S(v*tau)/tau.
5. Если level == L2:
   5a. Привести S(q) к выпуклой форме.
   5b. Применить Moreau или Tikhonov регуляризацию.
   5c. Пересчитать p(q), S(q), L(v).
6. Если level == L3:
   6a. Использовать фактические execution reports.
   6b. Калибровать impact-модель по данным исполнения.
   6c. Обновить S(q), p(q), L(v).
7. Посчитать epsilon1, epsilon2, epsilon3 и confidence.
8. Сформировать VenueLiquidityCurve.
9. Если включён режим compatibility:
   9a. Преобразовать VenueLiquidityCurve в SyntheticFlowOrder.
10. Вернуть [VenueLiquidityCurve, SyntheticFlowOrder?].
```

#### Алгоритм `BuildVirtualLOBFromAMM`

```text
Вход: dexPoolState, modelConfig

1. Получить текущую цену/тик и доступную ликвидность по диапазонам.
2. Для каждого tick-диапазона вычислить объём, доступный до пересечения следующего tick.
3. Построить набор виртуальных уровней (price, qty).
4. Сформировать виртуальный LOB.
5. Передать виртуальный LOB в стандартный ConvertLOBtoFOB.
```

#### Алгоритм `CircuitBreakerCheck`

```text
Вход: adapterState, newError

1. Добавить ошибку в скользящее окно ошибок.
2. Удалить ошибки старше CIRCUIT_BREAKER_WINDOW_S.
3. Если количество ошибок >= порога:
   3a. state = OPEN
   3b. publishedStatus = "circuit_open"
   3c. block external calls
4. Если state == OPEN и cooldown истёк:
   4a. state = HALF_OPEN
   4b. выполнить пробный запрос
   4c. если успех -> state = CLOSED
   4d. иначе -> state = OPEN повторно
5. Публиковать state и status в venue.health.
```

#### Алгоритм `StaleDetection`

```text
Вход: venueSnapshot, now, staleThresholdMs

1. age = now - venueSnapshot.timestamp
2. Если age > staleThresholdMs:
   2a. venueSnapshot.status = "stale"
   2b. запретить публикацию новых FOB-кривых для площадки
   2c. понизить confidence до 0 или до режима деградации
3. Иначе оставить status = "connected"
```

### 4.5. JSON‑модель данных
#### VenueSnapshot (Kafka `venue.snapshots`)

```json
{
  "snapshotId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "venueId": "binance",
  "venueType": "cex",
  "symbol": "BTCUSDT",
  "timestamp": "2026-03-13T10:00:00.123Z",
  "midPrice": 87250.50,
  "bestBid": 87250.00,
  "bestAsk": 87251.00,
  "spread": 1.00,
  "volume24h": 42150.75,
  "bidDepth": [
    {"price": 87250.00, "qty": 1.5},
    {"price": 87249.50, "qty": 3.2}
  ],
  "askDepth": [
    {"price": 87251.00, "qty": 1.8},
    {"price": 87251.50, "qty": 2.9}
  ],
  "fees": {"maker": 0.001, "taker": 0.001},
  "tickSize": 0.01,
  "lotSize": 0.001,
  "status": "connected"
}
```

#### VenueLiquidityCurve (Kafka `venue.liquidity.fob`)

```json
{
  "curveId": "f6af5b7f-8d23-4a11-90b3-7dbfae4b8b72",
  "venueId": "binance",
  "symbol": "BTCUSDT",
  "side": "buy",
  "level": "L2",
  "tauSec": 1.0,
  "qGrid": [0.0, 1.0, 2.0, 5.0, 10.0],
  "pOfQ": [87251.0, 87251.3, 87251.8, 87253.1, 87256.4],
  "sOfQ": [0.0, 87251.0, 174502.3, 436260.1, 872640.8],
  "vGrid": [0.0, 1.0, 2.0, 5.0, 10.0],
  "lOfV": [0.0, 87251.0, 174502.3, 436260.1, 872640.8],
  "epsilon1": 0.0042,
  "epsilon2": 0.0,
  "epsilon3": 0.012,
  "confidence": 0.94,
  "createdAt": "2026-03-13T10:00:00.180Z"
}
```

#### SyntheticFlowOrder (Kafka `venue.synthetic`)

```json
{
  "syntheticId": "f0e1d2c3-b4a5-6789-0fed-cba987654321",
  "venueId": "binance",
  "symbol": "BTCUSDT",
  "side": "buy",
  "pL": 87245.13,
  "pH": 87249.91,
  "qRate": 9.7,
  "qMax": 9.7,
  "curveId": "f6af5b7f-8d23-4a11-90b3-7dbfae4b8b72",
  "snapshotId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "createdAt": "2026-03-13T10:00:00.200Z",
  "expiresAt": "2026-03-13T10:00:01.200Z",
  "status": "active"
}
```

#### Venue Health (Kafka `venue.health`)

```json
{
  "venueId": "binance",
  "timestamp": "2026-03-13T10:00:00.500Z",
  "status": "connected",
  "latencyMs": 45,
  "snapshotsPerSec": 12.5,
  "errorRate": 0.0,
  "staleRate": 0.0,
  "circuitBreakerState": "CLOSED",
  "healthScore": 0.97,
  "lastSnapshotAge": 78
}
```

#### Execution Report (Kafka `execution.venue`)

```json
{
  "executionId": "87ac52d2-6b6d-4db1-81f0-a9c41d0fa4cc",
  "venueId": "binance",
  "symbol": "BTCUSDT",
  "side": "buy",
  "filledQty": 1.25,
  "avgPrice": 87252.10,
  "fee": 10.91,
  "status": "filled",
  "timestamp": "2026-03-13T10:00:00.650Z"
}
```

### 4.6. REST API
| Метод | Endpoint | Описание |
|-------|----------|----------|
| GET | `/api/v1/venues` | Список площадок с текущим статусом и health‑score |
| GET | `/api/v1/venues/{venueId}` | Детали площадки + последний VenueSnapshot |
| POST | `/api/v1/venues` | Добавить новую площадку |
| PUT | `/api/v1/venues/{venueId}` | Обновить конфигурацию площадки и модели LOB→FOB |
| DELETE | `/api/v1/venues/{venueId}` | Деактивировать площадку |
| POST | `/api/v1/venues/{venueId}/reconnect` | Force reconnect |
| GET | `/api/v1/venues/{venueId}/snapshots` | История VenueSnapshot |
| GET | `/api/v1/venues/{venueId}/curves` | История и текущие VenueLiquidityCurve |
| GET | `/api/v1/venues/{venueId}/synthetics` | Текущие SyntheticFlowOrder |
| GET | `/api/v1/venues/health` | Сводный status/health‑score по площадкам |

***
## 5. Тестирование
### 5.1. Автоматические тесты
#### Юнит‑тесты (External Venues Connector / Normalizer / Curve Builder)

Тестируются чистые функции:
- `normalizeSnapshot(rawData, config) -> VenueSnapshot`
- `convertLOBtoFOB(venueSnapshot, modelConfig) -> VenueLiquidityCurve`
- `buildVirtualLOBFromAMM(poolState, config) -> virtualLOB`
- `toSyntheticFlowOrder(curve) -> SyntheticFlowOrder`
- `circuitBreakerCheck(state, error) -> newState`

1. **Нормализация простого стакана CEX**  
   Вход: 5 bid‑уровней и 5 ask‑уровней.  
   Ожидание: mid/spread рассчитываются корректно, глубина сериализуется.

2. **Нормализация пустого стакана**  
   Вход: пустой bid/ask.  
   Ожидание: `VenueSnapshot.status = empty`.

3. **Нормализация DEX‑состояния**  
   Вход: tick/liquidity/sqrtPrice.  
   Ожидание: корректный `VenueSnapshot` для DEX/AMM.

4. **LOB→FOB Level 1 для симметричного стакана**  
   Ожидание: монотонная $p(q)$, корректная $S(q)$, валидная FOB‑кривая.

5. **LOB→FOB с учётом комиссий**  
   Ожидание: предельные цены и интегральная стоимость корректируются на fees.

6. **LOB→FOB с минимальным объёмом**  
   Ожидание: пылевые уровни фильтруются.

7. **Regularized L2**  
   Ожидание: после регуляризации выполняется монотонность/выпуклость, $\epsilon_2 = 0$.

8. **Calibrated L3**  
   Ожидание: модель корректно учитывает execution reports и обновляет $\epsilon_3$.

9. **DEX / AMM virtual LOB**  
   Ожидание: виртуальный LOB или FOB‑кривая строится без ошибок.

10. **Circuit breaker: CLOSED → OPEN**  
    Ожидание: после порогового числа ошибок площадка блокируется.

11. **Circuit breaker: OPEN → HALF_OPEN → CLOSED**  
    Ожидание: после cooldown и успешного probe площадка восстанавливается.

12. **Stale detection**  
    Ожидание: при превышении порога свежести новые кривые не строятся.

#### Интеграционные тесты (контейнеры + БД + Kafka)

1. **Полный цикл CEX**  
   Raw depth → VenueSnapshot → VenueLiquidityCurve → Matching.  
   Проверки: snapshot в ClickHouse, curve в `venue.liquidity.fob`, Matching видит внешнюю ликвидность.

2. **Полный цикл DEX/AMM**  
   Pool state → virtual LOB / FOB curve → Matching/ExecPlan.  
   Проверки: корректная публикация кривой и её использование.

3. **Execution hedge**  
   ExecutionIntent → Venue Execution Adapter → External Venues Connector → execution.venue → Ledger.  
   Проверки: корректное обновление venue‑балансов и PnL.

4. **Disconnect / Reconnect**  
   Проверки: stale → disconnected → reconnect → connected.

5. **Circuit breaker**  
   Проверки: OPEN/HALF_OPEN/CLOSED и публикация `venue.health`.

6. **Hot reload venue_config**  
   Проверки: изменение параметров модели LOB→FOB и подключения без рестарта сервиса.

7. **Multi‑venue по одному инструменту**  
   Проверки: несколько FOB‑кривых одновременно доступны Matching / Risk / ExecPlan.

### 5.2. Нагрузочные тесты
- Симуляция 10 одновременных площадок, по 20 снапшотов/сек на площадку;
- Целевой throughput: 200 `VenueSnapshot`/sec суммарно;
- Построение FOB‑кривых: p95 < 50 ms на один инструмент в L1/L2;
- Kafka consumer lag < 100 сообщений;
- ClickHouse ingestion не отстаёт.

### 5.3. Ручные тесты
**Функциональные:**
- Оператор добавляет площадку через Admin UI и видит новые `VenueSnapshot`, FOB‑кривые и health‑метрики;
- Оператор включает/отключает режим SyntheticFlowOrder compatibility;
- Оператор меняет модель LOB→FOB и наблюдает обновление quality‑метрик.

**Диагностические:**
- Проверка UI: status/health‑score/circuit breaker/freshness/curve preview;
- Проверка routing‑рекомендаций и их влияния на Execution Planning.

**Негативные:**
- Отключение mock‑server / RPC → stale → disconnected → alert;
- Подача аномального стакана/DEX‑состояния → валидация и отсутствие краша;
- Намеренное ухудшение качества данных → деградация L3 → L2 → L1 → OFF.

***
## 6. Definition of Done
- [ ] External Venues Connector подключается как минимум к 2 CEX и 1 DEX/AMM;
- [ ] Venue Market Data Normalizer публикует корректные `VenueSnapshot`;
- [ ] Venue Liquidity Curve Builder публикует `VenueLiquidityCurve` для активных площадок;
- [ ] В режиме совместимости генерируются `SyntheticFlowOrder`;
- [ ] Matching Backend использует внешнюю ликвидность в FOB‑форме;
- [ ] Risk Manager учитывает `venue.health` и качество внешней ликвидности;
- [ ] Execution Planning & Forecast использует FOB‑кривые и health‑score для routing;
- [ ] Venue Execution Adapter публикует `execution.venue` и Ledger корректно обновляет venue‑балансы;
- [ ] Stale detection реализован и протестирован;
- [ ] Circuit breaker реализован и протестирован для каждого venue;
- [ ] Admin API / CRUD `venue_config` и hot reload работают;
- [ ] p95 задержка `VenueSnapshot` < 500 ms;
- [ ] p95 LOB→FOB < 50 ms для L1/L2;
- [ ] Ошибка стоимости LOB→FOB не превышает 1% на рабочем диапазоне объёмов;
- [ ] Пройдены unit/integration/load tests;
- [ ] Дашборд оператора показывает статусы площадок, health‑score, stale rate, quality‑метрики FOB‑кривых;
- [ ] Документация API актуальна;
- [ ] Code review пройден, ветка вмержена.

## 7. Дополнительные пояснения

### 1. Состояния circuit breaker

1. **CLOSED**  
   - Venue считается **здоровым** и полностью доступным для хеджа и маркет‑даты.  
   - External Venues Connector и Execution Adapter свободно отправляют market‑запросы и ExecutionIntent / child orders на этот venue. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - Ошибки всё равно считаются, но пока их доля ниже порога — breaker остаётся в CLOSED. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - Событие `venue.health` публикуется с `status=CLOSED`, когда venue выходит из проблемного состояния. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

2. **OPEN**  
   - Venue считается **недоступным / небезопасным**: за окно наблюдения зафиксирована серия ошибок (timeouts, 5xx, invalid payload, rate‑limit и т.п.). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - Все новые ExecutionIntent / внешние хедж‑ордера на этот venue **блокируются или перенаправляются** на другие venues; допускаются только health‑check’и. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
   - Состояние ставится при превышении порога ошибок и публикуется в `venue.health` с причиной (`error_burst`, `timeout_spike`, и т.п.). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
   - До истечения cooldown никаких попыток «нормального» использования venue не делается.

3. **HALF_OPEN**  
   - Переходное состояние: venue «под наблюдением», выполняются **ограниченные пробные запросы**. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - Разрешается небольшой объём health‑check / тестовых запросов (например, один небольшой ордер или лёгкий LOB‑запрос) для проверки, восстановилось ли API. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - Если пробные запросы проходят успешно — breaker возвращается в **CLOSED**; если снова фиксируются ошибки — возвращается в **OPEN** с новым cooldown. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
   - Все переходы (`OPEN → HALF_OPEN → CLOSED/OPEN`) также транслируются в `venue.health` и в UI оператору. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

Это сценарий работы **circuit breaker’а для внешнего venue** в рамках фичи **F‑11 External Venues / venue.health**. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

## 2. Сценарий: Circuit breaker

### 1. Серия ошибок за окно наблюдения

- Компонент **External Venues Connector / Venue Execution Adapter** делает запросы к API площадки: market data, ордера, отмены. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
- Для каждого venue ведётся счётчик/метрика ошибок за скользящее окно (HTTP‑ошибки, timeouts, invalid payload, rate‑limit). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
- Как только доля ошибок превышает порог (например, N ошибок за M секунд), **Venue Health & Routing Service** фиксирует, что venue «нестабилен» и инициирует смену состояния. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

По сути это детектор: «API этой биржи сейчас сломано/флапает, надо перестать туда слать хедж».

### 2. Перевод в `OPEN` и блокировка

- Состояние circuit breaker’а по venue переводится в `OPEN`. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
- В этом состоянии **все новые ExecutionIntent / внешние ордера на этот venue отклоняются или маршрутизируются на другие venues**; External Venues Connector перестаёт слать туда новые запросы, кроме служебных health‑check. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
- Событие публикуется в Kafka‑топик `venue.health` с полями `status=OPEN`, `reason=error_burst`, `timestamp`, `metrics` и т.д. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

Таким образом вы мгновенно «отрубаете» проблемную площадку, не давая ей портить хедж и PnL.

### 3. Cooldown и `HALF_OPEN`

- После истечения заданного **cooldown** (например, 30–60 секунд) Venue Health & Routing Service переключает breaker в режим `HALF_OPEN`. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
- В `HALF_OPEN` разрешаются **ограниченные тестовые запросы**: один или несколько пробных вызовов (пинг, мини‑запрос LOB или маленький ордер), но ещё не полноценный поток хедж‑заявок. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

Это шаг «проверяем, ожила ли биржа, но рискуем минимально».

### 4. Успех/неудача пробного запроса

- **При успехе** (ответ в срок, корректные данные, без критических ошибок):  
  - breaker переводится в состояние `CLOSED` — venue считается здоровым;  
  - нормальный поток ExecutionIntent и market data восстанавливается. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
- **При неудаче** (таймаут, 5xx, некорректный payload и т.п.):  
  - breaker остаётся в `OPEN` и запускается новый период cooldown;  
  - venue продолжает считаться недоступным для хеджа и маршрутизации. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

Логика полностью соответствует классическому паттерну circuit breaker.

### 5. Публикация в `venue.health` и UI

- Все переходы (`CLOSED → OPEN → HALF_OPEN → CLOSED/OPEN`) логируются и публикуются как события `venue.health` в Kafka. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
- Эти события:  
  - используются **Execution Planning & Forecast** и **Risk Manager** при выборе venue и оценке риска исполнения; [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
  - сохраняются в ClickHouse для анализа отказов venues; [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
  - отображаются в **Web UI / Admin UI**: оператор видит статус площадки (цвет/иконка, причина, время последнего перехода, счётчики ошибок). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md)

В итоге F‑11 + venue.health дают автоматический **«аварийный выключатель» для внешней ликвидности**: при проблемах на CEX/DEX вы не продолжаете слать туда симметричные заявки, а автоматически отключаете venue до восстановления.

## Сценарий: DEX / AMM без классического стакана
1. Adapter получает состояние пула (например, `sqrtPriceX96`, `tick`, `liquidity`, ticks для Uniswap v3).
2. Venue Market Data Normalizer формирует `VenueSnapshot` для DEX/AMM.
3. Venue Liquidity Curve Builder синтезирует виртуальный LOB/непосредственно FOB‑кривую из состояния пула.
4. Далее процесс идентичен CEX: нормализация → FOB‑кривая → клиринг/риск/планирование.

«Без стакана» здесь значит: у DEX/AMM **нет классического биржевого order book (LOB)** с видимыми лимитными заявками bid/ask по уровням цены. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md)

У CEX:

- есть **стакан**: список заявок «купить N по цене P», «продать M по цене Q», упорядоченный по цене/времени;  
- внешний адаптер просто читает LOB и превращает его в VenueSnapshot и FOB‑кривую. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

У DEX/AMM:

- нет явных лимитных ордеров, есть **формула пула** (xy‑k, stableswap, v3‑пул с `sqrtPriceX96`, `tick`, `liquidity` и т.п.);  
- стакан не публикуется, его нужно **синтезировать математически**: по состоянию пула вычислить, какой объём можно купить/продать по каждому уровню цены. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

Поэтому в формулировке:

> «DEX/AMM без стакана можно встроить как ещё один venue…»

имеется в виду: даже если площадка **не отдаёт LOB в явном виде**, мы через F‑11 (адаптер + нормализатор + Liquidity Curve Builder) умеем получить эквивалент LOB/FOB‑кривой и дальше обрабатывать DEX **точно так же**, как CEX (Market Data → FOB‑кривая → Matching/Risk/Hedge). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

Суть: этот сценарий по F‑11 проверяет, что **DEX/AMM без стакана можно встроить в систему как ещё один venue, не ломая весь остальной контур (Market Data → FOB‑кривая → Matching/Risk/Hedge)**. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

### Что происходит по шагам

1. **Adapter получает состояние пула**  
   - Venue Execution Adapter для DEX читает не L2‑стакан, а параметры AMM‑пула: `sqrtPriceX96`, `tick`, `liquidity`, массив ticks/позиции для Uniswap v3 или аналогичные параметры для другого AMM. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - Это «сырой» on‑chain state вместо классического LOB.

2. **Venue Market Data Normalizer → VenueSnapshot**  
   - На основе этих параметров нормализатор строит `VenueSnapshot` для DEX/AMM: вычисляет текущую цену, спред (если есть), доступные объёмы в окрестности цены, служебные поля (timestamp, venue, symbol). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md)
   - Таким образом DEX становится *ещё одним источником рыночных данных* в унифицированном формате VenueSnapshot, как CEX. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

3. **Venue Liquidity Curve Builder → виртуальный LOB / FOB‑кривая**  
   - Builder, зная формулу AMM (xy‑k, stableswap, piecewise v3 и т.п.), «разворачивает» состояние пула в **непрерывную кривую «цена–объём»**: какую цену мы получим при покупке/продаже разных объёмов. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - Получается либо виртуальный LOB, либо сразу `VenueLiquidityCurve`/SyntheticFlowOrder в формате, который понимает FOB‑ядро. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

4. **Дальше всё как с CEX**  
   - На этом месте DEX/AMM уже «выглядит» как обычный venue: есть VenueSnapshot, есть FOB‑кривая. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - Далее тот же pipeline:  
     - Market Data Service использует кривые в reference prices / аналитике.  
     - Matching Backend может учитывать внешнюю ликвидность в батч‑клиринге.  
     - Risk Manager и Execution Planning & Forecast оценивают impact/стоимость исполнения и планируют хедж/маршрутизацию. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md)

### Зачем мы это проверяем

- **Единый интерфейс venues.** F‑11 должен доказать, что CEX и DEX/AMM обрабатываются по одной и той же схеме «сырой поток → VenueSnapshot → VenueLiquidityCurve», без форков логики в backend’е. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
- **Математика AMM → FOB.** Мы убеждаемся, что формулы для преобразования AMM‑состояния в FOB‑кривую корректны, дают разумные цены/объёмы и хорошо интегрируются с CSLO‑клирингом. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
- **Расширяемость.** Если этот сценарий проходит, вы можете добавлять новые AMM/DEX как «ещё один venue‑адаптер», не переписывая Matching, Risk, Ledger и Execution Hedge. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

Проще: этот сценарий доказывает, что ваша архитектура venue‑слоя действительно **абстрагирует источник ликвидности**, и DEX/AMM становится полноценным первым классом гражданином наряду с CEX.



## Сценарий: Пустая или нулевая ликвидность
1. Для площадки отсутствует bid/ask или доступная ликвидность в рабочем диапазоне.
2. `VenueSnapshot` получает статус `empty`.
3. FOB‑кривая и `SyntheticFlowOrder` не публикуются.
4. Увеличивается счётчик `venue.empty_snapshots`.

Суть сценария: проверить, что система **корректно обрабатывает venue без торгуемой ликвидности** и не пытается использовать её в клиринге/хедже, а также экспонирует это состояние наружу через метрики и health. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

### Что происходит по шагам

1. **Нет bid/ask или доступного объёма**  
   - External Venues Connector и Venue Market Data Normalizer получают данные по venue (LOB с CEX или состояние AMM‑пула). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - В рабочем диапазоне цен не находится ни одного уровня bid/ask с ненулевым объёмом (или AMM‑пул в таком состоянии, что нужный объём исполнить нельзя). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - Фактически для данного инструмента на этой площадке **ликвидности нет**.

2. **`VenueSnapshot` → `status=empty`**  
   - Вместо «нормального» снапшота с ценами/объёмами нормализатор помечает VenueSnapshot как `empty` (например, `mode=empty` или `status=empty`). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
   - Это явный сигнал всем downstream‑компонентам: «на этом venue по этому символу сейчас торговать нельзя».

3. **FOB‑кривая / SyntheticFlowOrder не публикуются**  
   - Venue Liquidity Curve Builder **не строит** `VenueLiquidityCurve` и не генерирует SyntheticFlowOrder для этого venue/symbol. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
   - В результате Matching Backend, Execution Planning и Risk Manager **даже не видят** внешнюю ликвидность для этого venue по данному инструменту и не могут её случайно использовать. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

4. **Счётчик `venue.empty_snapshots` растёт**  
   - Для venue/instrument увеличивается метрика `venue.empty_snapshots` (например, в Prometheus/ClickHouse). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
   - По этой метрике Observability & Reporting и Venue Health & Routing Service могут:  
     - строить графики «часто ли venue пустой по этому инструменту»;  
     - принимать решения по routing (не планировать хедж туда, где почти всегда пусто);  
     - подсвечивать в UI, что по этому venue инструмент сейчас «illiquid / empty». [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

### Зачем проверяем этот сценарий

- **Безопасность клиринга и хеджа.** Нельзя допустить, чтобы Matching/Risk/Execution Planning опирались на «фиктивную» внешнюю ликвидность, которой нет; этот сценарий гарантирует, что при пустом venue никакие FOB‑кривые и synthetic‑ордера не появятся. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
- **Правильные сигналы для routing.** Счётчик `venue.empty_snapshots` и статус `empty` в VenueSnapshot позволяют системе и операторам понимать, что venue по этому инструменту фактически неиспользуемый, и учитывать это в стратегиях маршрутизации. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
- **Универсальность обработки.** Мы проверяем, что и CEX (нет заявок), и DEX/AMM (пул не даёт объём в диапазоне) приводятся к одному и тому же поведению: `status=empty`, нет FOB‑кривой, нет хеджа. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

Нужна ли вам аналогичная расшифровка для ещё какого‑то сценария по F‑11 (например, «аномальный спред» или «резкий обвал цены на venue»)?

## Сценарий: Несколько площадок для одного инструмента**
1. Подключены несколько venues для одного инструмента.
2. Каждая площадка публикует собственный `VenueSnapshot` и `VenueLiquidityCurve`.
3. Matching / Risk / ExecPlan используют все доступные источники с учётом health‑score.
4. Market Data Service строит агрегированные BBO/reference prices.

Суть сценария: проверить, что **один и тот же инструмент может одновременно торговаться на нескольких venues**, и система корректно использует все источники ликвидности — для клиринга, риска, хеджа и reference‑цен. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

### Что происходит по шагам

1. **Подключены несколько venues для одного инструмента**  
   Для, скажем, BTCUSDT одновременно активны несколько внешних площадок (CEX1, CEX2, DEX/AMM и т.п.), каждая со своим API, комиссиями и качеством ликвидности. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

2. **Каждый venue даёт свой `VenueSnapshot` и `VenueLiquidityCurve`**  
   - External Venues Connector и Venue Market Data Normalizer формируют по каждому venue отдельный `VenueSnapshot` (mid, bid/ask, глубина и т.п.). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
   - Venue Liquidity Curve Builder по каждому снапшоту строит свою FOB‑кривую `VenueLiquidityCurve` (или SyntheticFlowOrder) — набор «цена–объём» для этого venue. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

3. **Matching / Risk / ExecPlan используют все источники с учётом health‑score**  
   - Matching Backend, Risk Manager и Execution Planning & Forecast видят **набор** кривых по одному инструменту: `{CEX1_curve, CEX2_curve, DEX_curve, ...}` плюс `venue.health`/health‑score каждого. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - На основе этих данных они решают: куда лучше направить хедж, как распределить объём между venues, кого игнорировать (если health плохой или ликвидность пустая/дорогая). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

4. **Market Data Service строит агрегированные BBO/reference prices**  
   - Market Data Service агрегирует все VenueSnapshot по инструменту и строит **сводные** BBO/reference‑цены (например, лучшая bid/ask по всем venues, VWAP, средневзвешенные mid‑цены и др.). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md)
   - Эти агрегаты используются в UI, риск‑моделях и планировании исполнения как «рыночная» цена инструмента, а не цена одной конкретной биржи. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md)

### Зачем проверяем этот сценарий

- **Мультивеню‑архитектура.** Сценарий подтверждает, что ваша система — действительно multi‑venue: один инструмент опирается на несколько источников ликвидности, а не на один выбранный CEX. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
- **Качество исполнения и риска.** Возможность одновременно учитывать все VenueLiquidityCurve и их health‑score критична для оптимального хеджа, снижения impact и распределения риска между площадками. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
- **Корректная агрегация цен.** Сценарий гарантирует, что Market Data Service строит reference‑цены на основе **всех** доступных venues, а не случайно «забывает» часть из них, что важно для правильного PnL, маржи и UI. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/56e58d56-579d-4849-9605-97c1ba65caa4/Poiasnitelnaia-zapiska.md)

Хотите, чтобы я отдельно расписал, какие простые правила агрегации вы можете использовать для BBO/reference (min ask, max bid, mid‑среднее, VWAP по объёму и т.п.)?

##  Изменяемые объекты конфигурации
- Таблица `venue_config` в PostgreSQL:
  - сетевые настройки площадки;
  - tick/lot/fees;
  - список символов;
  - параметры reconnect/circuit breaker;
  - конфигурация LOB→FOB модели;
- Kafka topics и retention policy;
- Environment variables:
  - `STALE_THRESHOLD_MS`;
  - `CIRCUIT_BREAKER_ERRORS`;
  - `CIRCUIT_BREAKER_WINDOW_S`;
  - `CIRCUIT_BREAKER_COOLDOWN_S`;
  - `LOB_TO_FOB_DEFAULT_LEVEL`;
  - `LOB_TO_FOB_TAU_SEC`.

  Это список «ручек», которыми вы управляете поведением F‑11 и внешнего контура. Ниже пояснения по каждому изменяемому объекту.

***

### Таблица `venue_config` в PostgreSQL

Это центральный конфигурационный объект для каждой внешней площадки (CEX/DEX/AMM). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

- **Сетевые настройки площадки**  
  Хосты, порты, протоколы (REST/WebSocket/RPC), URL эндпоинтов, таймауты, параметры авторизации (API‑ключи, headers, rate‑limits). Меняя их, вы можете переключить venue на другой endpoint, включить sandbox/live, настроить таймауты запросов, не перекатывая код. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

- **tick/lot/fees**  
  Минимальный шаг цены (tick size), минимальный и кратный размер ордера (lot size), торговые комиссии (taker/maker, доп. надбавки/скидки). Это нужно для: [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)
  - корректной нормализации LOB/AMM в FOB‑кривую (учёт тиков/лотов);  
  - расчёта эффективной цены и slippage с учётом комиссий.

- **Список символов**  
  Перечень инструментов, которые подключены на этом venue (например, `["BTCUSDT", "ETHUSDT"]`), плюс, возможно, маппинг внутренних `instrumentinternalid` ↔ внешние `symbol`. Меняя список, вы включаете/отключаете инструменты без изменения кода адаптера. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

- **Параметры reconnect / circuit breaker**  
  Локальные параметры отказоустойчивости по venue: максимальное число попыток reconnect, паузы между ними, локальный порог ошибок для срабатывания circuit breaker (если вы хотите переопределить глобальные env). Это позволяет более агрессивно или наоборот мягко обращаться с конкретной биржей. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

- **Конфигурация LOB→FOB модели**  
  Параметры, как именно из LOB/AMM строится FOB‑кривая:  
  - глубина по объёму/уровням,  
  - режим агрегации,  
  - использование конкретной модели (например, piecewise, сглаживание и т.п.). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)
  Это позволяет тонко настроить, как именно venue представляется ядру FOB, без модификации Matching/Risk.

***

### Kafka topics и retention policy

Конфигурируются сами топики и их retention. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

- Какие топики используются:  
  - `venue.snapshots` — VenueSnapshot,  
  - `venue.liquidity.fob` — FOB‑кривые,  
  - `venue.synthetic` — SyntheticFlowOrder,  
  - `venue.health` — health и routing рекомендации,  
  - `execution.venue` — отчёты о внешнем исполнении. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

- **Retention policy** (время/объём хранения):  
  - более короткий retention для высокочастотных потоков (LOB/FOB), чтобы не раздувать кластер;  
  - более длинный retention для `venue.health`/`execution.venue`, если они нужны для расследований и отчётности;  
  - возможная разная политика для продакшн/тестовых топиков. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

Это позволяет балансировать между «историей» для backtest/отчётов и нагрузкой на Kafka.

***

### Environment variables (глобальные пороги/тайминги)

Эти переменные задают **глобальные** параметры поведения HEALTH/LOB→FOB. Они могут переопределяться per‑venue через `venue_config`, но по умолчанию действуют для всех. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

- **`STALE_THRESHOLD_MS`**  
  Через сколько миллисекунд после последнего снапшота по venue данные считаются **устаревшими (stale)**.  
  - Если между снапшотами прошло больше этого значения, Health помечает venue как stale/disconnected, Curve Builder может перестать публиковать кривые, а Risk/ExecPlan — понижать приоритет. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

- **`CIRCUIT_BREAKER_ERRORS`**  
  Сколько ошибок (или доля ошибок) нужно за окно, чтобы сработал circuit breaker.  
  - Например, 5 ошибок подряд или N ошибок из последних M запросов. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

- **`CIRCUIT_BREAKER_WINDOW_S`**  
  Длина окна в секундах, в пределах которого считаются ошибки для circuit breaker.  
  - Определяет чувствительность к «всплескам» ошибок: маленькое окно → быстрые срабатывания, большое окно → более инерционное поведение. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

- **`CIRCUIT_BREAKER_COOLDOWN_S`**  
  Время в секундах, которое venue проводит в состоянии `OPEN` перед переходом в `HALF_OPEN` для пробного запроса.  
  - Регулирует, как часто система будет «пробовать восстановиться» после отключения venue. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

- **`LOB_TO_FOB_DEFAULT_LEVEL`**  
  Базовый уровень / глубина, до которой LOB конвертируется в FOB‑кривую, если нет override в `venue_config`.  
  - Например, использовать top‑N уровней стакана или объём до X% от типичного дневного оборота. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)

- **`LOB_TO_FOB_TAU_SEC`**  
  Временной параметр модели LOB→FOB (условный «горизонт» или сглаживающий timescale), который влияет на то, как сильно учитываются быстрые колебания/шумы. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)
  - Например, при большем `TAU` кривая более сглаженная и отражает «устойчивую» ликвидность, при меньшем — более чувствительна к моментальному состоянию стакана.

***

Вместе эти объекты конфигурации дают вам:

- `venue_config` — **per‑venue** настройки сети, лимитов, модели LOB→FOB и поведения отказоустойчивости;  
- Kafka topics + retention — управление **потоками данных и их историей**;  
- env‑переменные — глобальные **пороговые значения и тайминги** для staleness и circuit breaker, а также дефолты для LOB→FOB.

Хотите, чтобы я оформил это в виде небольшой таблицы «параметр → влияние на систему» для прямой вставки в ТЗ?
  
## Описания тестов

Ниже — развернутые идеи, что именно проверять и как.

***

### 1. Нормализация простого стакана CEX

**Цель:** убедиться, что обычный LOB с CEX корректно превращается в `VenueSnapshot`: правильно считаются mid/spread и сериализуются уровни глубины. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)

**Вход:**

- 5 bid‑уровней: \((price_i, size_i)\), убывающие по цене.  
- 5 ask‑уровней: \((price_j, size_j)\), возрастающие по цене.  
- Заданы tick/lot/fees для venue (из `venue_config`). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)

**Тест‑кейсы:**

1. **Расчёт bestBid / bestAsk / mid / spread**

   - `bestBid = max(bid.price)`  
   - `bestAsk = min(ask.price)`  
   - `mid = (bestBid + bestAsk) / 2`  
   - `spread = bestAsk - bestBid`  

   Проверяем, что:

   - В `VenueSnapshot.bestBid` и `bestAsk` попали именно крайние уровни.  
   - `mid` и `spread` совпадают с эталонным расчётом (с учётом формата округления, если он задан). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)

2. **Сериализация глубины**

   - Проверяем, что в `VenueSnapshot` попал массив bid/ask‑уровней:  
     - отсортирован по цене в нужном направлении;  
     - содержит ровно 5 уровней или отсечён/дополнен по правилам (например, top‑N);  
     - каждый уровень уважает tick/lot (цены кратны tick, количества кратны lot). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

3. **Фи‑модель (если учитывается)**

   - Если нормализатор сразу учитывает комиссии (effective price), проверяем, что корректно посчитаны:  
     - `effectiveBid = bestBid * (1 - takerFee)`  
     - `effectiveAsk = bestAsk * (1 + takerFee)`  
   - И что эти поля (или производные) корректно отражены в snapshot (если предусмотрены). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)


**Входной LOB (BTCUSDT):**

Bid‑уровни (цена, размер):

- bid1: 69 900, size 0.8  
- bid2: 69 850, size 1.2  
- bid3: 69 800, size 0.5  
- bid4: 69 750, size 2.0  
- bid5: 69 700, size 1.0  

Ask‑уровни:

- ask1: 70 000, size 0.6  
- ask2: 70 050, size 1.0  
- ask3: 70 100, size 0.4  
- ask4: 70 150, size 1.5  
- ask5: 70 200, size 2.0  

Пусть tick size = 10 USDT, lot size = 0.1 BTC (все уровни валидны по tick/lot). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)

**Ожидаемый `VenueSnapshot`:**

- `bestBid = 69_900`  
- `bestAsk = 70_000`  
- `mid = (69_900 + 70_000) / 2 = 69_950`  
- `spread = 70_000 - 69_900 = 100`  

Глубина:

- массив `bids` из 5 элементов, отсортированный по убыванию цены:  
  `[(69900, 0.8), (69850, 1.2), (69800, 0.5), (69750, 2.0), (69700, 1.0)]`  
- массив `asks` из 5 элементов, по возрастанию цены:  
  `[(70000, 0.6), (70050, 1.0), (70100, 0.4), (70150, 1.5), (70200, 2.0)]` [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)

Если тестируете суммарную глубину до ±100 USDT от mid:

- диапазон цен: \([69_850; 70_050]\)  
- по bid внутри диапазона: 69 900 (0.8), 69 850 (1.2) → суммарно 2.0 BTC;  
- по ask внутри диапазона: 70 000 (0.6), 70 050 (1.0) → суммарно 1.6 BTC.

Можно ожидать поля типа:

- `bidDepthNearMid = 2.0`  
- `askDepthNearMid = 1.6` (если такие агрегаты считаете). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/b26f488a-1702-4edb-bf9e-67e20c21fd7b/Vazhnye-zamechaniia.md)

***

### 2. Нормализация пустого стакана

**Цель:** гарантировать, что при отсутствии заявок venue не выдаётся как торгуемый — snapshot помечается `empty` и дальше правильно фильтруется. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

**Вход:**

- Пустые массивы bid и ask: `bids = []`, `asks = []`.  
- Либо массивы есть, но все `size = 0` или за пределами рабочего диапазона.

**Ожидания:**

1. **Статус snapshot**

   - В `VenueSnapshot.status` (или эквивалентном поле) строго стоит `empty` (либо `mode=empty`). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)
   - Все поля, зависящие от стакана (bestBid, bestAsk, mid, spread, depth), либо `null`, либо не заполнены по правилам формата.

2. **Отсутствие FOB‑кривой**

   В рамках F‑11 дальше по цепочке (в отдельном интеграционном тесте):

   - Curve Builder, получая `status=empty`, **не публикует** `VenueLiquidityCurve` и SyntheticFlowOrder для этого venue/symbol. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
   - В метриках health увеличивается `venue.empty_snapshots` (но это уже сценарий «Пустая или нулевая ликвидность»). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

#### Вариант A: полностью пустой LOB

**Вход:**

- `bids = []`  
- `asks = []`

**Ожидания:**

- `VenueSnapshot.status = "empty"`  
- `bestBid = null`, `bestAsk = null`, `mid = null`, `spread = null` (или не заполнены).  
- массивы `bids` и `asks` пустые. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)

#### Вариант B: нулевая ликвидность

**Вход:**

- `bids = [(69_900, 0.0)]`  
- `asks = [(70_000, 0.0)]`

По бизнес‑правилу — treat as empty:

- `VenueSnapshot.status = "empty"`  
- кривые/FOB‑ликвидность не строятся. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)



***

### 3. Нормализация DEX‑состояния

**Цель:** проверить, что AMM‑пул без явного стакана корректно превращается в эквивалент `VenueSnapshot` (цена, спред/эффективный спред, доступный объём) и не ломает общий формат. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

**Вход:**

- Набор параметров пула (пример для Uniswap v3):  
  - `sqrtPriceX96`,  
  - `tick`,  
  - `liquidity`,  
  - массив `ticks[]` с ликвидностью по диапазонам.  
- Конфигурация пула: пары токенов, fee‑tier, tick spacing и т.п.

**Тест‑кейсы:**

1. **Расчёт текущей цены**

   - Из `sqrtPriceX96`/`tick` вычисляем ожидаемую цену \(\text{price}_{expected}\) по формуле AMM.  
   - Проверяем, что `VenueSnapshot.mid` (или `price`) совпадает с \(\text{price}_{expected}\) с нужной точностью. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

2. **Оценка эффективного bid/ask / спреда**

   В AMM спред задаётся фи‑моделью/кривой, но в snapshot часто нужно вывести «эффективный» bid/ask:

   - Вычисляем, по какой цене можно:  
     - купить малый объём \(\Delta Q\) токена X за Y → эффективный ask;  
     - продать \(\Delta Q\) X за Y → эффективный bid.  
   - Сравниваем с полями `bestBid`/`bestAsk` в `VenueSnapshot`. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

3. **Оценка доступной ликвидности**

   - Для заданного диапазона цен или объёмов рассчитываем, сколько можно купить/продать, и проверяем, что поля глубины (или эквивалентная метрика liquidity) в `VenueSnapshot` соответствуют этим расчётам. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

4. **Формат и совместимость**

   - Проверяем, что DEX‑snapshot полностью совместим по структуре с CEX‑snapshot:  
     - те же обязательные поля (venue, symbol, ts, mid/price, статус);  
     - корректные типы (числа/строки), сериализация и т.п. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/3d746db7-a468-4af2-8b9f-295239c7c40a/Fichi.md)
   - Это важно, чтобы downstream‑компоненты (MDS, Curve Builder, Risk, Plan) могли потреблять DEX и CEX без ветвления логики.

Возьмём классический x*y = k пул BTC/USDT (упрощённо, без fee):

Пусть в пуле:

- $x = 100$ BTC  
- $y = 7000000$ USDT  

Тогда:

- $k = x \cdot y = 100 \cdot 7_000_000 = 700_000_000$ 

**Текущая цена BTC в USDT:**

Цена для infinitesimal buy/sell ≈ $y / x$:

- $price = y / x = 7_000_000 / 100 = 70_000$ USDT за 1 BTC.

**Ожидаемый `VenueSnapshot`:**

- `mid ≈ 70_000` (можно записать как price).  
- Если хотите задать псевдо‑спред 10 USDT:  
  - `bestBid ≈ 69_995`  
  - `bestAsk ≈ 70_005`  
  - `spread ≈ 10`  
  либо храните только mid и отдельное поле impact/fee. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

**Пример оценки ликвидности вокруг mid:**

Смоделируем, сколько USDT нужно, чтобы купить 1 BTC из пула.

- До сделки: $x_0 = 100$, $y_0 = 7_000_000$, $k = 700_000_000$.  
- Хотим купить $\Delta x = 1$ BTC, тогда новый x: $x_1 = x_0 - \Delta x = 99$.  
- Новый y определяется из $x_1 \cdot y_1 = k$:  
  $y_1 = k / x_1 = 700_000_000 / 99 ≈ 7_070_707.07$.  
- Потрачено USDT: $\Delta y = y_1 - y_0 ≈ 7_070_707.07 - 7_000_000 ≈ 70_707.07$.  

Средняя цена покупки 1 BTC:

$$
price_{buy,avg} ≈ 70_707.07 \text{ USDT}
$$

Это даёт **импакт ≈ 707 USDT** относительно mid 70 000.

Тест можно упростить:

- ожидать, что нормализатор:  
  - выставит `mid ≈ 70_000`,  
  - посчитает эффективный buy‑price на 1 BTC ≈ 70 700 (при округлении),  
  - может записать это либо в отдельное поле impact/liquidity, либо отразить как «псевдо‑ask» выше mid. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/collection_a975fb64-0a47-409b-9232-18b5a86e9ca9/44688595-8efb-4238-81c9-9d807076fd1b/sformiruite-biznes-trebovaniia-HEUK3VufQ0aHIZQnWmVWDA.md)

**Пример поля liquidity:**

- Можно в `VenueSnapshot` добавить агрегат:  
  - `maxBuyQtyAtImpact1pct = q`, такую, что средняя цена не выходит за 1% от mid.  
- В нашем примере impact для 1 BTC уже ≈ 1%, значит:  
  - `maxBuyQtyAtImpact1pct ≈ 1 BTC`.  
- Это число можно использовать как проверяемую метрику в тесте: расчёт из модели должен совпасть.

***

Если будете действительно закладывать в код AMM‑пример, я бы зафиксировал в тесте:

- вход: $x=100, y=7_000_000$, запрос на 1 BTC;  
- ожидаемый `mid ≈ 70_000`, `price_buy_avg ≈ 70_707` (допуск по погрешности, например ±1 USDT),  
чтобы regression‑тест защищал формулу.



***

### 4. LOB→FOB Level 1 для симметричного стакана

**Цель:** проверить, что быстрая L1‑аппроксимация строит корректную монотонную \(p(q)\), интегральную стоимость \(S(q)\) и валидный `VenueLiquidityCurve`. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

**Пример входа (ask‑сторона, BTCUSDT):**

Упорядоченный LOB:

- Уровень 1: цена 70 000, объём 1 BTC  
- Уровень 2: цена 70 010, объём 1 BTC  
- Уровень 3: цена 70 020, объём 1 BTC  

Берём сетку по объёму \(q \in \{0, 1, 2, 3\}\) BTC.

**Ожидаемое \(p(q)\) (ступенчатая функция):**

- \(p(0) = 70\,000\) (конвенция — первая предельная цена)  
- Для \(0 < q \le 1\): \(p(q) = 70\,000\)  
- Для \(1 < q \le 2\): \(p(q) = 70\,010\)  
- Для \(2 < q \le 3\): \(p(q) = 70\,020\)

Проверки:

- \(p(q)\) **монотонно неубывает** по \(q\) (никаких «ступенек вниз»). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)
- Значения \(p(q)\) совпадают с соответствующими уровнями LOB.

**Интегральная стоимость \(S(q)\):**

По определению \(S(q) = \int_0^q p(x)\,dx\). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

Для нашей дискретной сетки:

- \(S(0) = 0\)  
- \(S(1) = 70\,000 \cdot 1 = 70\,000\)  
- \(S(2) = 70\,000 \cdot 1 + 70\,010 \cdot 1 = 140\,010\)  
- \(S(3) = 140\,010 + 70\,020 \cdot 1 = 210\,030\)

Ожидания:

- Значения \(S(q)\) **строго возрастают**.  
- При численной реализации (сумма ступеней) различие с эталоном ≤ небольшой допуск (из‑за float).

**FOB‑кривая (валидность):**

- `VenueLiquidityCurve` содержит:  
  - сетку \(q\) (например, `[0,1,2,3]`),  
  - массив `p(q)` монотонный,  
  - массив `S(q)` строго возрастающий. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)
- Все значения удовлетворяют базовым инвариантам (нет отрицательных цен, нет отрицательной стоимости, нет NaN).

***

## 5. LOB→FOB с учётом комиссий

**Цель:** проверить, что при добавлении fee модель корректно смещает предельные цены и интегральную стоимость. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

**Пример входа:**

Берём тот же ask‑LOB:

- уровни: 70 000, 70 010, 70 020, объёмы по 1 BTC.

Комиссия taker fee: 0.1% (0.001).

**Ожидаемые эффективные цены:**

Для покупателя эффективный ask:

$$
p_{\text{eff}}(q) = p(q) \cdot (1 + fee)
$$

Значит ступени для \(p_{\text{eff}}\):

- \(p_{\text{eff}}(0<q\le1) = 70\,000 \cdot 1.001 = 70\,070\)  
- \(p_{\text{eff}}(1<q\le2) = 70\,010 \cdot 1.001 ≈ 70\,080.01\)  
- \(p_{\text{eff}}(2<q\le3) = 70\,020 \cdot 1.001 ≈ 70\,090.02\)

**Интегральная стоимость с комиссиями:**

- \(S_{\text{eff}}(1) = 70\,070\)  
- \(S_{\text{eff}}(2) = 70\,070 + 70\,080.01 ≈ 140\,150.01\)  
- \(S_{\text{eff}}(3) ≈ 140\,150.01 + 70\,090.02 ≈ 210\,240.03\)

Ожидания:

- Кривая \(p_{\text{eff}}(q)\) остаётся монотонной.  
- \(S_{\text{eff}}(q)\) > \(S(q)\) на величину, соответствующую комиссии.  
- В `VenueLiquidityCurve` явно видно смещение цен/стоимости относительно без‑fee варианта (можно проверять разницу). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

***

### 6. LOB→FOB с минимальным объёмом

**Цель:** убедиться, что «пылевые» уровни с объёмом ниже минимально значимого порога не попадают в FOB‑кривую и не ломают $p(q)\)/\(S(q)$. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

**Пример входа:**

Пусть:

- Минимальный значимый объём для FOB: `minVolume = 0.1 BTC`.  

Ask‑LOB:

- lvl1: 70 000, size 0.01 BTC (пыль)  
- lvl2: 70 010, size 0.05 BTC (пыль)  
- lvl3: 70 020, size 0.3 BTC (нормальный)  
- lvl4: 70 030, size 0.7 BTC (нормальный)

**Ожидаемое поведение:**

- Уровни с size < 0.1 BTC **отфильтровываются**.  
- Эффективный LOB для LOB→FOB:

  - lvl1': 70 020, size 0.3  
  - lvl2': 70 030, size 0.7

Тогда:

- для \(0<q\le0.3\): \(p(q)=70\,020\)  
- для \(0.3<q\le1.0\): \(p(q)=70\,030\).

Интегральная стоимость:

- \(S(0.3) = 70\,020 \cdot 0.3 = 21\,006\)  
- \(S(1.0) = 21\,006 + 70\,030 \cdot 0.7 ≈ 70\,027\).

Тест проверяет:

- пылевые уровни **не влияют** на \(p(q)\) и \(S(q)\);  
- нет случайного «скачка» по цене из‑за tiny‑уровней;  
- `VenueLiquidityCurve` использует только фильтрованный LOB. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

***

### 7. Regularized L2

**Цель:** проверить, что в режиме Level 2 после регуляризации выполняются монотонность/выпуклость, а ошибка по выпуклости \( \epsilon_2 = 0 \) (в пределах допуска). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

**Пример входа (LOB с артефактом):**

Пусть по ask:

- lvl1: 70 000, size 1  
- lvl2: 70 005, size 1  
- lvl3: 69 990, size 1  ← «глючный» уровень ниже предыдущей цены  
- lvl4: 70 020, size 1  

Наивная L1‑аппроксимация даст ступень \(p(q)\), которая **не монотонна** (из‑за 69 990 после 70 005).

**Ожидаемое поведение L2:**

- L2 делает регуляризацию (например, через Moreau/Tikhonov или просто исчисление монотонной выпуклой оболочки):  
  - исправляет порядок цен так, чтобы \(p(q)\) не убывала;  
  - делает \(S(q)\) выпуклой функцией. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

Простейший ожидаемый результат:

- Исправленная последовательность цен, например: 70 000, 70 005, 70 005, 70 020  
  (или 70 000, 70 005, 70 010, 70 020 — зависит от конкретного алгоритма, но **без убывания**).  

Проверки:

1. **Монотонность:**  
   - для сетки \(q_i\) проверяем \(p(q_{i+1}) \ge p(q_i)\) для всех i (учитывая допуск).

2. **Выпуклость \(S(q)\):**  
   - из \(S(q)\) считаем вторые разности:  
     \(\Delta^2 S(q_i) = S(q_{i+1}) - 2 S(q_i) + S(q_{i-1})\);  
   - ожидаем \(\Delta^2 S(q_i) \ge 0\) (в пределах численного допуска). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

3. **Метрика \( \epsilon_2 \):**  
   - в модели L2 \( \epsilon_2 \) — ошибка по выпуклости (или невыполнению ограничений);  
   - тест ожидает \( \epsilon_2 \approx 0 \) (либо строго 0, если метрика дискретная).  
   Например, если \( \epsilon_2 \) реализована как max violation, проверяем \( \epsilon_2 < 10^{-8} \). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

***

### 8. Calibrated L3

**Цель:** проверить, что Level 3 корректно использует execution reports, обновляет impact‑модель и метрику ошибки исполнения \( \epsilon_3 \). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

**Идея примера:**

1. **Исходная L2‑кривая (до калибровки):**

Пусть L2 даёт для покупки 1 BTC:

- прогнозируемая средняя цена исполнения \(p_{\text{model}} = 70\,000\)  
- т.е. ожидаемый \(S_{\text{model}}(1) = 70\,000\).

2. **Фактическое исполнение:**

Из `execution.venue` приходят отчёты, показывающие, что:

- фактическая средняя цена была 70 200 (более дорогой рынок из‑за скрытого impact/fees);  
- значит фактическая стоимость \(S_{\text{real}}(1) = 70\,200\). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

3. **Ошибка исполнения до калибровки:**

$$
\epsilon_3^{\text{before}} = |S_{\text{model}}(1) - S_{\text{real}}(1)| = |70\,000 - 70\,200| = 200.
$$

4. **Ожидаемое поведение L3:**

- L3 должна **подстроить** параметры impact‑модели (например, добавить линейный/квадратичный термин) так, чтобы на следующем шаге:  
  - новая кривая \(p_{\text{calibrated}}(q)\) давала стоимость \(S_{\text{calibrated}}(1)\) ближе к 70 200. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

**Проверки:**

1. После обработки execution reports:

   - \(S_{\text{calibrated}}(1)\) изменился в сторону фактического значения (то есть \(|S_{\text{calibrated}}(1) - 70\,200| < |70\,000 - 70\,200|\)).  
   - Идеально: \(S_{\text{calibrated}}(1) ≈ 70\,200\) (с заданным допускаемым отклонением).

2. \(\epsilon_3\) уменьшилась:

$$
\epsilon_3^{\text{after}} = |S_{\text{calibrated}}(1) - S_{\text{real}}(1)| < \epsilon_3^{\text{before}} = 200.
$$

3. При этом L3 **не ломает** инварианты L2:

- \(p_{\text{calibrated}}(q)\) остаётся монотонной,  
- \(S_{\text{calibrated}}(q)\) — выпуклой (иначе калибровка недопустима). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/22c7918a-6f5c-4496-b415-f8c0b16cdf91/F-11_corrected.md?AWSAccessKeyId=ASIA2F3EMEYEY4HXAUUB&Signature=5SC9qgKoMfLEV8g16yx2Q%2B%2BIoRM%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEOf%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCXVzLWVhc3QtMSJHMEUCIC%2BprXpIeKe9GrYlVu0lTy9vNnwpl9D6KOds42aAneEmAiEA0uO4XG8InlhTFdBdfpQI6jUgDQjN6R2bqlHldDOjkYYq%2FAQIsP%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FARABGgw2OTk3NTMzMDk3MDUiDK0Tkhx5pswtwvLpBSrQBLyXhicNuW%2FE1oS1oHC4FFqmyVsVX5aTxOb2FJfnsCer1KE187VQLaXM91VtZb1fCLbzeHYWbm3uWg6Mrtfnz2KueW0%2BXVTYxW5fmT8p2z%2FMxzLeY9%2FkZIskTif3DqKVMWbXsGDHXZhMRlmoh4PvpE2ZjXTaC1RIZYMLl30dNFZvlyPkzs8sYkoCwlSoCvsYzd2oU%2FrJXyr3WgFC9YhGyNrlmS1D0nzOREDcKq8WwpinH%2BhcIhn%2FPTSkPFsvejL4gWtAoW8%2FKFjBsiWPBc22n0uPR9FCePnNgrdJ5tBi%2BQZhV%2Fr3SXQQ9wp4yEgRYAg0xjy6wvgSgnfoAcbLEbyRez7V%2B%2BhjfmdfEyxmBtwXta4n75vuF%2Frn3yXEF2UY6YodMHtbKaPsaS4Cslw5TaBbRPnJxF%2F18T7LgZ%2FH85w1oTcqQ%2BlQtU8gGL1F1rwL5Kd2Bu9mX%2BX5UyuujaygqF4A%2F7gWEnShUJIYTPBMfVqo080l%2BdZdO5LX5kyhUVvy%2FwVe%2BgG9uGQbPxFL9bbxyo1dkJV7boZjezkxmp0cT8cSID0fF6EbGo4xbRt%2FZCO7NCoJXc6Zu4n0T1LqvSNpf3SUi65GT5MnPwSWe53oKDt1bI64YiSOQlED81HqPG5oPpaUg2IiEGjFDoF8lTbZB1PiPwQlqRzNiUTjrrk1TPMFoSezZ81tvf0qTlldkm4%2BRl4HcAa24dvMV8ixST2JADCI1rDRhEzkHflZ7JyRYL7YHkBahLqLGO4HvYIZaUt9vcswpnkSEAGiHbqkhP1OVbHfnCEw5%2BqPzgY6mAHuX6aazfGQwIoMXnihCLXUjsHuMDW6iMkm1lEj8SaevzNuxa5Ok02mMiA6s6rxe4M1GFFUsd0nb7b3Kj5AVecnl6ey1B8dpjMTUzL5o1ciZr86SgYmIKGWoRVephYEG3mtrYCqR5jItgWs4pwYkMYYlRwkhINd12llc%2FTtnrhQp9kHye4g6rApWBShH31DEsWdZMy6Q3TStA%3D%3D&Expires=1774450950)

***

