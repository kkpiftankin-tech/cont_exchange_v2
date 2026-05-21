# Note on encoding

This document was received via chat attachment on 2026-05-20. The Cyrillic content arrived as UTF-8 mojibake (rendered as Latin-1). The structural content (Mermaid diagrams, JSON, SQL, formulas, code identifiers, English terms) is intact. Prose Russian content is mojibake-encoded; readers should decode via Python `bytes.decode('utf-8', errors='replace')` or apply `latin1 -> utf-8` round-trip.

Original document title: **F-11. Подключение внешних площадок (CEX/DEX)**

Per CLAUDE.md immutable-archive rule, this file is the immutable source. Normalized artifacts produced from this source live under `docs/02-system/features/F-11-external-venues-lob-to-fob/`, `docs/05-components/`, `docs/06-api/`, `docs/07-data/`, etc.

---

# Структурные артефакты, извлечённые из IN-004 (encoding-independent)

## Канонические термины (отдекодированы)

- **Venue (площадка)** — внешняя биржа или пул ликвидности (CEX, DEX, AMM)
- **External Venues Connector** — низкоуровневый компонент подключения к площадкам (WebSocket/REST/RPC, heartbeat, reconnect)
- **Venue Market Data Normalizer** — нормализация сырых данных в `VenueSnapshot`
- **VenueSnapshot** — нормализованный снимок внешнего рынка
- **Venue Liquidity Curve Builder** — компонент перевода LOB/AMM → FOB-кривую
- **VenueLiquidityCurve** — непрерывное FOB-представление внешней ликвидности
- **SyntheticFlowOrder** — виртуальная FlowOrder из VenueLiquidityCurve
- **Venue Health & Routing Service** — оценка качества и здоровья площадок
- **Venue Execution Adapter** — исполнение внешних child-ордеров/хеджа
- **Execution Planning & Forecast** — онлайн-планирование исполнения
- **LOB (Limit Order Book)** — дискретный стакан заявок
- **FOB (Flow Order Book)** — непрерывное представление ликвидности

## Формулы нормализации

$$
\text{mid} = \frac{\text{bestBid} + \text{bestAsk}}{2}
$$

$$
\text{spread} = \text{bestAsk} - \text{bestBid}
$$

## LOB-to-FOB цепочка преобразования

1. Из дискретных уровней стакана строится функция предельной цены исполнения $p(q)$ как функция кумулятивного объёма $q$
2. По ней строится интегральная стоимость исполнения:

$$
S(q) = \int_0^q p(x)\,dx
$$

3. При необходимости $S(q)$ делается выпуклой и гладкой через регуляризацию
4. Через временной масштаб $\tau$ строится скорость исполнения:

$$
v = \frac{q}{\tau}
$$

5. Строится FOB-представление по скорости:

$$
L(v) = \frac{S(v\tau)}{\tau}
$$

или excess-cost вариант:

$$
L(v) = \frac{S(v\tau) - p_{ref} \cdot v\tau}{\tau}
$$

6. Публикуется как `VenueLiquidityCurve`

### Режимы LOB-FOB
- **Level 1 (Fast)** — быстрая монотонная инженерная аппроксимация
- **Level 2 (Regularized)** — выпуклая и сглаженная кривая с регуляризацией (Moreau/Tikhonov)
- **Level 3 (Calibrated)** — кривая, дополнительно откалиброванная по фактическим исполнениям

## Kafka-топики

| Топик | Назначение |
|---|---|
| `venue.snapshots` | VenueSnapshot |
| `venue.liquidity.fob` | VenueLiquidityCurve |
| `venue.synthetic` | SyntheticFlowOrder |
| `venue.health` | health-score и статусы соединений |
| `execution.venue` | execution reports |

## ClickHouse: venue_snapshots

| Поле | Тип | Описание |
|------|-----|----------|
| `snapshotid` | UUID | Уникальный ID снимка |
| `venueid` | String | Идентификатор площадки |
| `symbol` | String | Торговый инструмент |
| `timestamp` | DateTime64 | UTC-время события |
| `midprice` | Float64 | mid-цена |
| `bestbid` | Float64 | Лучшая цена покупки |
| `bestask` | Float64 | Лучшая цена продажи |
| `spread` | Float64 | Спред |
| `volume24h` | Float64 | Объём 24ч |
| `biddepth` | String (JSON) | Глубина bid |
| `askdepth` | String (JSON) | Глубина ask |
| `fees` | String (JSON) | Комиссии |
| `ticksize` | Float64 | Шаг цены |
| `lotsize` | Float64 | Шаг объёма |
| `status` | Enum | connected/stale/disconnected/empty |

## ClickHouse: venue_liquidity_curves

| Поле | Тип | Описание |
|------|-----|----------|
| `curveid` | UUID | Уникальный ID кривой |
| `venueid` | String | Площадка |
| `symbol` | String | Инструмент |
| `side` | Enum | buy/sell |
| `level` | Enum | L1/L2/L3 |
| `tau_sec` | Float64 | Временной масштаб |
| `q_grid` | String (JSON) | Сетка объёмов |
| `p_of_q` | String (JSON) | Предельная цена |
| `s_of_q` | String (JSON) | Интегральная стоимость |
| `l_of_v` | String (JSON) | FOB-кривая по скорости |
| `epsilon1` | Float64 | Ошибка стоимости |
| `epsilon2` | Float64 | Ошибка монотонности |
| `epsilon3` | Float64 | Ошибка исполнения |
| `confidence` | Float64 | Доверие к кривой |
| `createdat` | DateTime64 | Время публикации |

## PostgreSQL: venue_config

```sql
CREATE TABLE venue_config (
  venueid           VARCHAR(32) PRIMARY KEY,
  venuetype         venuetype_enum,  -- 'cex'|'dex'|'amm'
  displayname       VARCHAR(128),
  apiurl            TEXT,
  wsurl             TEXT,
  symbols           JSONB,
  pollingintervalms INT,
  reconnectattempts INT,
  reconnectdelayms  INT,
  fees              JSONB,
  ticksize          JSONB,
  lotsize           JSONB,
  lobtofobmodel     JSONB,
  isactive          BOOLEAN,
  createdat         TIMESTAMPTZ,
  updatedat         TIMESTAMPTZ
);
```

## PostgreSQL: synthetic_orders

```sql
CREATE TABLE synthetic_orders (
  syntheticid  UUID PRIMARY KEY,
  venueid      VARCHAR(32) REFERENCES venue_config(venueid),
  symbol       VARCHAR(32),
  side         side_enum,  -- 'buy'|'sell'
  pl           NUMERIC(24,8),
  ph           NUMERIC(24,8),
  qrate        NUMERIC(24,8),
  qmax         NUMERIC(24,8),
  curveid      UUID,
  snapshotid   UUID,
  createdat    TIMESTAMPTZ,
  expiresat    TIMESTAMPTZ,
  status       synthetic_status_enum  -- 'active'|'expired'|'used'
);
```

## Функциональные требования (F11-1..F11-20)

- **F11-1.** Подключение к CEX по WebSocket/REST API
- **F11-2.** Подключение к DEX/AMM через RPC/event subscription
- **F11-3.** Построение VenueSnapshot для каждого события
- **F11-4.** Публикация VenueSnapshot в Kafka venue.snapshots + ClickHouse
- **F11-5.** Отдельный компонент Venue Liquidity Curve Builder
- **F11-6.** Поддержка L1/L2/L3 режимов LOB-FOB
- **F11-7.** L1: монотонная аппроксимация p(q), S(q), L(v)
- **F11-8.** L2: выпуклость + регуляризация Moreau/Tikhonov + epsilon1, epsilon2
- **F11-9.** L3: калибровка impact-модели по execution reports + epsilon3
- **F11-10.** Публикация VenueLiquidityCurve в Kafka venue.liquidity.fob
- **F11-11.** Производное построение SyntheticFlowOrder из VenueLiquidityCurve
- **F11-12.** Matching Backend использует внешнюю ликвидность только в FOB-форме
- **F11-13.** Risk Manager учитывает venue.health
- **F11-14.** Execution Planning использует VenueLiquidityCurve и venue.health
- **F11-15.** Venue Execution Adapter: ExecutionIntent → child-orders → execution.venue
- **F11-16.** Stale detection: при превышении stalethresholdms — не строить новых FOB-кривых
- **F11-17.** Circuit breaker per venue с CLOSED/OPEN/HALF_OPEN
- **F11-18.** DEX/AMM без классического стакана: синтез виртуального LOB
- **F11-19.** Admin API CRUD для venue_config с hot reload
- **F11-20.** Логирование версий моделей LOB-FOB для backtest/replay

## Нефункциональные требования

- p95 латентность получения VenueSnapshot < 500 ms
- Throughput: ≥ 100 VenueSnapshot/sec суммарно
- Построение FOB-кривой L1/L2: p95 < 50 ms на один инструмент
- venue.health публикуется не позднее 1 сек после инцидента
- Ошибка стоимости LOB→FOB epsilon1 ≤ 1% на рабочем диапазоне
- История VenueSnapshot и VenueLiquidityCurve в ClickHouse ≥ 90 дней
- Hot reload venue_config без перезапуска сервиса
- Graceful degradation L3 → L2 → L1 → OFF при ухудшении данных

## REST API

| Метод | Endpoint | Описание |
|-------|----------|----------|
| GET | /api/v1/venues | Список площадок со статусами |
| GET | /api/v1/venues/{venueId} | Детали площадки + последний VenueSnapshot |
| POST | /api/v1/venues | Добавить площадку |
| PUT | /api/v1/venues/{venueId} | Обновить конфигурацию |
| DELETE | /api/v1/venues/{venueId} | Деактивировать |
| POST | /api/v1/venues/{venueId}/reconnect | Force reconnect |
| GET | /api/v1/venues/{venueId}/snapshots | История VenueSnapshot |
| GET | /api/v1/venues/{venueId}/curves | История VenueLiquidityCurve |
| GET | /api/v1/venues/{venueId}/synthetics | Текущие SyntheticFlowOrder |
| GET | /api/v1/venues/health | Сводный health-score |

## JSON-схемы (примеры)

### VenueSnapshot (Kafka venue.snapshots)

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
  "bidDepth": [{"price": 87250.00, "qty": 1.5}],
  "askDepth": [{"price": 87251.00, "qty": 1.8}],
  "fees": {"maker": 0.001, "taker": 0.001},
  "tickSize": 0.01,
  "lotSize": 0.001,
  "status": "connected"
}
```

### VenueLiquidityCurve (Kafka venue.liquidity.fob)

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

### Venue Health (Kafka venue.health)

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

## Environment variables

- `STALE_THRESHOLD_MS`
- `CIRCUIT_BREAKER_ERRORS`
- `CIRCUIT_BREAKER_WINDOW_S`
- `CIRCUIT_BREAKER_COOLDOWN_S`
- `LOB_TO_FOB_DEFAULT_LEVEL`
- `LOB_TO_FOB_TAU_SEC`

## Алгоритм Circuit Breaker

- **CLOSED**: venue здоров, отправка ExecutionIntent разрешена
- **OPEN**: серия ошибок за окно → блокировка orders, fallback на другие venues
- **HALF_OPEN**: после cooldown → ограниченные пробные запросы; success → CLOSED, fail → OPEN

## Definition of Done (F-11)

- [ ] External Venues Connector подключается минимум к 2 CEX и 1 DEX/AMM
- [ ] Venue Market Data Normalizer публикует корректные VenueSnapshot
- [ ] Venue Liquidity Curve Builder публикует VenueLiquidityCurve
- [ ] В режиме совместимости генерируются SyntheticFlowOrder
- [ ] Matching Backend использует внешнюю ликвидность в FOB-форме
- [ ] Risk Manager учитывает venue.health
- [ ] Execution Planning использует FOB-кривые и health-score для routing
- [ ] Venue Execution Adapter публикует execution.venue, Ledger обновляет venue-балансы
- [ ] Stale detection реализован и протестирован
- [ ] Circuit breaker реализован per venue
- [ ] Admin API CRUD venue_config + hot reload
- [ ] p95 задержка VenueSnapshot < 500 ms
- [ ] p95 LOB-FOB < 50 ms для L1/L2
- [ ] Ошибка стоимости LOB-FOB ≤ 1% на рабочем диапазоне объёмов
- [ ] Пройдены unit/integration/load tests
- [ ] Dashboard оператора показывает статусы, health-score, quality-метрики
- [ ] Документация API актуальна
- [ ] Code review пройден

---

## Тестовые кейсы (структурно)

### Unit-тесты
1. Нормализация простого стакана CEX (5 bid + 5 ask)
2. Нормализация пустого стакана → status=empty
3. Нормализация DEX-состояния (sqrtPriceX96, tick, liquidity для Uniswap v3)
4. LOB-FOB Level 1 для симметричного стакана
5. LOB-FOB с учётом комиссий
6. LOB-FOB с минимальным объёмом (фильтрация пылевых уровней)
7. Regularized L2 (выпуклость, монотонность, epsilon2=0)
8. Calibrated L3 (учёт execution reports, обновление epsilon3)
9. DEX/AMM virtual LOB
10. Circuit breaker: CLOSED → OPEN
11. Circuit breaker: OPEN → HALF_OPEN → CLOSED
12. Stale detection (пересечение порога свежести)

### Интеграционные тесты
1. Полный цикл CEX: Raw depth → VenueSnapshot → VenueLiquidityCurve → Matching
2. Полный цикл DEX/AMM: Pool state → virtual LOB / FOB curve → Matching/ExecPlan
3. Execution hedge: ExecutionIntent → Adapter → EVC → execution.venue → Ledger
4. Disconnect/reconnect cycle
5. Circuit breaker full state machine
6. Hot reload venue_config
7. Multi-venue для одного инструмента

### Нагрузочные тесты
- 10 одновременных площадок, 20 снапшотов/сек на площадку
- Целевой throughput: 200 VenueSnapshot/sec
- Построение FOB-кривых: p95 < 50 ms на инструмент в L1/L2
- Kafka consumer lag < 100 сообщений
