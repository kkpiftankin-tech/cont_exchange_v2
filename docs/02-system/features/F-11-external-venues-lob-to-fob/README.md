# F-11 — External Venues / LOB → FOB

> **Статус:** in-progress, MVP. Connectors к Binance/Coinbase/Uniswap v3 + LOB→FOB конвертация + Venue Health/Routing работают; ClickHouse DDL + risk-консьюмер health — pending.

## Бизнес-цели

Подключить внешнюю ликвидность (CEX/DEX/AMM) к Continuous Exchange как непрерывный источник `VenueLiquidityCurve`, чтобы:

1. matching solver мог использовать внешний рынок наравне с внутренними FlowOrder (F-04, F-09);
2. Execution Planning мог хеджировать остаточные позиции по best venue (F-12);
3. оператор мог наблюдать качество и доступность каждой площадки в реальном времени (F-16, F-17);
4. система могла авто-блокировать деградировавшие venue (circuit breaker) и переключаться на резерв.

## Ключевые сущности

- **Venue** — внешняя площадка (CEX, DEX, AMM). Конфиг — в PostgreSQL `venue_config`.
- **VenueSnapshot** — нормализованный снимок ордербука. Proto: [`fob.venue.v1.VenueSnapshot`](../../../../contracts/proto/fob/venue/v1/venue.proto). Kafka topic: `venue.snapshots`.
- **VenueLiquidityCurve** — непрерывное FOB-представление: $p(q)$, $S(q)$, $L(v)$, плюс дуальная пара $S^{*}(p)$, $q^{*}(p)$. Proto: `fob.venue.v1.VenueLiquidityCurve`. Kafka topic: `venue.liquidity.fob`.
- **SyntheticFlowOrder** — производная виртуальная FlowOrder, материализованная из кривой; поддерживается для совместимости с matching v1. Kafka topic: `venue.synthetic`.
- **VenueHealth** — health-score, latency, error rate, circuit-breaker state. Kafka topic: `venue.health` (RAW от cpp/venues + AGGREGATED от cpp/venue_health).

## Алгоритм LOB → FOB

Из уровней стакана строится функция предельной цены исполнения $p(q)$, далее интегральная стоимость:

$$
S(q) = \int_{0}^{q} p(x)\,dx
$$

Через временной масштаб $\tau$ кривая в координатах скорости:

$$
v = \frac{q}{\tau}, \qquad L(v) = \frac{S(v\tau)}{\tau}
$$

или excess-cost вариант относительно reference price $p_{\text{ref}}$:

$$
L(v) = \frac{S(v\tau) - p_{\text{ref}} \cdot v\tau}{\tau}
$$

Поддерживаются три уровня модели:

- **L1 (Fast)** — монотонная инженерная аппроксимация.
- **L2 (Regularized)** — выпуклая + Moreau/Tikhonov; контроль `epsilon1`, `epsilon2`.
- **L3 (Calibrated)** — L2 + калибровка по фактическим execution reports; контроль `epsilon3`.

Деградация: $L3 \to L2 \to L1 \to \text{OFF}$ при ухудшении данных (метрика `last_curve_degradation_reason`).

## Компоненты

| Компонент                                                                                        | Runtime              | Назначение                                                                    |
| ------------------------------------------------------------------------------------------------ | -------------------- | ----------------------------------------------------------------------------- |
| [external-venues-connector](../../../05-components/external-venues-connector/)                   | `cpp/venues`         | низкоуровневое подключение (WS/REST/RPC), heartbeat, reconnect                |
| [venue-market-data-normalizer](../../../05-components/venue-market-data-normalizer/)             | `cpp/venues`         | сырые данные → `VenueSnapshot`, snapshot_status, depth_canonicalizer          |
| [venue-liquidity-curve-builder](../../../05-components/venue-liquidity-curve-builder/)           | `cpp/venues`         | LOB→FOB (L1/L2/L3), SyntheticFlowOrder, dual layer                            |
| [venue-health-routing](../../../05-components/venue-health-routing/)                             | `cpp/venue_health`   | aggregated health-score, circuit breaker FSM, routing recommendation         |
| [venue-execution-adapter](../../../05-components/venue-execution-adapter/)                       | `cpp/venues`         | `ExecutionIntent` → child orders → `ExecutionReport` (бизнес-логика в F-12)   |

## Реализация (текущий статус)

### CEX (Binance / Coinbase)

- WebSocket + REST через [cex_ws_rest_adapter.cpp](../../../../cpp/venues/src/infra/cex_ws_rest_adapter.cpp).
- Локальная сборка стакана из инкрементов: [cex_local_lob_assembler.cpp](../../../../cpp/venues/src/infra/cex_local_lob_assembler.cpp).

### DEX/AMM (Uniswap v3)

- RPC + event-subscription через [dex_amm_rpc_adapter.cpp](../../../../cpp/venues/src/infra/dex_amm_rpc_adapter.cpp).
- Virtual LOB из pool state (`sqrtPriceX96`, `tick`, `liquidity`): [amm_pool_extractor.cpp](../../../../cpp/venues/src/domain/amm_pool_extractor.cpp), [amm_virtual_lob.cpp](../../../../cpp/venues/src/domain/amm_virtual_lob.cpp).
- Прямое построение FOB-кривой из AMM-инварианта: [amm_direct_fob.cpp](../../../../cpp/venues/src/domain/amm_direct_fob.cpp).

### Симулятор (для CI/dev)

- [simulated_venue_adapter.cpp](../../../../cpp/venues/src/infra/simulated_venue_adapter.cpp), [venue_sim_adapter.cpp](../../../../cpp/venues/src/infra/venue_sim_adapter.cpp).

### Admin API + hot reload

- HTTP сервер на Crow, порт `VENUES_ADMIN_HTTP_PORT=8087` ([main.cpp](../../../../cpp/venues/src/main.cpp)).
- Полный CRUD + reconnect/enable/disable/routing-mode.
- Hot reload: `VenuesLoop::UpsertVenueConfig` → `apply_runtime_config_locked` без рестарта; см. [venues_loop.cpp](../../../../cpp/venues/src/app/venues_loop.cpp).

### Venue Health & Routing

- Отдельный сервис `cpp/venue_health`: consume `venue.health` (RAW) → `VenueState` → publish AGGREGATED.
- Circuit breaker FSM: [circuit_breaker.cpp](../../../../cpp/venue_health/src/domain/entities/circuit_breaker.cpp).
- Env: `CIRCUIT_BREAKER_ERRORS`, `CIRCUIT_BREAKER_WINDOW_S`, `CIRCUIT_BREAKER_COOLDOWN_S`, `STALE_THRESHOLD_MS`.

## Use Cases

| Use Case                                                                                                                          | Назначение                                                              |
| --------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------- |
| [UC-F11-01. Onboard venue](../../use-cases/UC-F11-01-onboard-venue/use-case.md)                                                   | Operator подключает новую площадку через Admin UI / API                  |
| [UC-F11-02. Publish VenueSnapshot](../../use-cases/UC-F11-02-publish-snapshot/use-case.md)                                        | Connector → Normalizer → Kafka venue.snapshots                          |
| [UC-F11-03. Build VenueLiquidityCurve](../../use-cases/UC-F11-03-build-liquidity-curve/use-case.md)                               | LOB → FOB (L1/L2/L3) → venue.liquidity.fob (+ venue.synthetic)          |
| [UC-F11-04. Venue health degradation](../../use-cases/UC-F11-04-venue-health-degradation/use-case.md)                             | Stale / circuit breaker open / fallback                                 |
| [UC-F11-05. Execute hedge on venue](../../use-cases/UC-F11-05-execute-hedge-on-venue/use-case.md)                                 | (cross-link F-12) Adapter принимает ExecutionIntent                     |
| [UC-F11-01-ingest-external-marketdata](../../use-cases/UC-F11-01-ingest-external-marketdata/use-case.md)                          | (legacy stub) — заменён UC-F11-02; оставлен для совместимости ссылок     |

## Sequence Diagrams

System-level (`UC-F11-*/sequences/`):

- [SEQ-UC-F11-01-system](../../use-cases/UC-F11-01-onboard-venue/sequences/SEQ-UC-F11-01-system.md) — Operator ↔ System (onboarding)
- [SEQ-UC-F11-02-system](../../use-cases/UC-F11-02-publish-snapshot/sequences/SEQ-UC-F11-02-system.md) — CEX/DEX ↔ System (snapshot)
- [SEQ-UC-F11-03-system](../../use-cases/UC-F11-03-build-liquidity-curve/sequences/SEQ-UC-F11-03-system.md) — внутренний (snapshot → curve)
- [SEQ-UC-F11-04-system](../../use-cases/UC-F11-04-venue-health-degradation/sequences/SEQ-UC-F11-04-system.md) — Operator/Trader ↔ System (health alerts)
- [SEQ-UC-F11-05-system](../../use-cases/UC-F11-05-execute-hedge-on-venue/sequences/SEQ-UC-F11-05-system.md) — System ↔ Venue (execution)

Service-level (`docs/05-components/sequences/`):

- [SEQ-F11-01-onboard-venue-services](../../../05-components/sequences/SEQ-F11-01-onboard-venue-services.md)
- [SEQ-F11-02-publish-snapshot-services](../../../05-components/sequences/SEQ-F11-02-publish-snapshot-services.md)
- [SEQ-F11-03-build-curve-services](../../../05-components/sequences/SEQ-F11-03-build-curve-services.md)
- [SEQ-F11-04-health-routing-services](../../../05-components/sequences/SEQ-F11-04-health-routing-services.md)
- [SEQ-F11-05-execute-on-venue-services](../../../05-components/sequences/SEQ-F11-05-execute-on-venue-services.md)

## Контракты

- [venue-topics.md](../../../06-api/messaging/venue-topics.md) — Kafka топики (`venue.snapshots`, `venue.liquidity.fob`, `venue.synthetic`, `venue.health`, `execution.venue`).
- [venues.md (REST)](../../../06-api/rest/venues.md) — Admin REST API.

## Данные

- [venue-config](../../../07-data/venue-config.md) — PostgreSQL.
- [synthetic-orders](../../../07-data/synthetic-orders.md) — PostgreSQL.
- [venue-snapshots](../../../07-data/venue-snapshots.md) — ClickHouse, retention ≥ 90 дней.
- [venue-liquidity-curves](../../../07-data/venue-liquidity-curves.md) — ClickHouse.

## Acceptance Criteria

См. [acceptance-criteria.md](acceptance-criteria.md).

## Open Questions

См. [open-questions.md](open-questions.md).

## Conflict Notes

### C-1. Curve Builder как отдельный компонент vs внутри `cpp/venues`

**Источник:** IN-004 §«Канонические термины» описывает `Venue Liquidity Curve Builder` как отдельный компонент.

**Реальность:** В коде `LiquidityCurveProducer` живёт в [cpp/venues/src/app/liquidity_curve_producer.cpp](../../../../cpp/venues/src/app/liquidity_curve_producer.cpp) и собирается в единый бинарь `venues` вместе с Connector и Normalizer.

**Решение:** В docs-as-code артефактах компонент **`venue-liquidity-curve-builder`** оформлен отдельно (component.yaml + overview.md + sequences), но `runtime: cpp/venues`. См. [ADR-014-venues-binary-vs-components](../../../03-architecture/adr/ADR-014-venues-binary-vs-components.md). Решение обратимо: split на отдельный сервис может быть выполнен T-F11-300 при необходимости масштабирования.

### C-2. Дублирование `marketdata.raw` и `venue.snapshots`

**Источник:** IN-004 §«Kafka-топики» декларирует `venue.snapshots` как новый канон для F-11.

**Реальность:** [venues_loop.cpp](../../../../cpp/venues/src/app/venues_loop.cpp) публикует одни и те же данные в legacy `marketdata.raw` (для F-05) **и** в `venue.snapshots` (для F-11). Дублирование оправдано переходным периодом.

**Решение:** Постепенная миграция consumers F-05 на `venue.snapshots`. Plan — T-F11-310. До тех пор оба топика остаются.

### C-3. `venue.health` — два producer'а

**Источник:** IN-004 описывает `venue.health` как односторонний топик от Health Service.

**Реальность:** Поле `VenueHealthEventType` в proto явно разделяет `RAW` (от `cpp/venues`) и `AGGREGATED` (от `cpp/venue_health`); оба пишут в один топик. Consumer Health Service фильтрует по event_type.

**Решение:** Документировано в proto и в [venue-topics.md](../../../06-api/messaging/venue-topics.md). Альтернатива (`venue.health.raw` + `venue.health.aggregated`) — обратимо через ADR при возникновении проблем.

## Связанные фичи

- **F-04 (Batch Clearing)** — потребитель `venue.liquidity.fob` через external_venue_filter.
- **F-05 (Live Market Data)** — legacy потребитель `marketdata.raw`; постепенно мигрирует на `venue.snapshots`.
- **F-09 (Combo/Portfolio Orders)** — потребитель SyntheticFlowOrder для multi-leg.
- **F-12 (Execution Hedge)** — основной потребитель `execution.venue` и venue.liquidity.fob; владеет Execution Planning.
- **F-15 (Backtest/Replay)** — использует `backtest.execution.venue` и `schema_version`/`producer_version` для детерминированного replay.
- **F-16 (Operator Console)** — UI для Admin API.
- **F-17 (Monitoring)** — потребитель `venue.health` + Prometheus metrics.

## Source Fragments

- IN-004 (F-11. Подключение внешних площадок CEX/DEX) — единственный источник.
- См. [incoming-docs/IN-004.meta.md](../../../../incoming-docs/IN-004.meta.md), [incoming-docs/IN-004.fragment-map.md](../../../../incoming-docs/IN-004.fragment-map.md).
