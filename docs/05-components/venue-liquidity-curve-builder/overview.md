# Компонент: venue-liquidity-curve-builder

> **Runtime:** `cpp/venues` (один бинарь с Connector + Normalizer + Execution Adapter, см. [ADR-014](../../03-architecture/adr/ADR-014-venues-binary-vs-components.md)).

## Назначение

Конвертация LOB → FOB. Builder:

1. Принимает `VenueSnapshot` от [Normalizer](../venue-market-data-normalizer/overview.md).
2. Строит $p(q)$ (предельная цена), $S(q) = \int_{0}^{q} p(x)\,dx$ (интегральная стоимость), $L(v) = S(v\tau)/\tau$ (FOB по скорости) и дуальный слой $S^{*}(p)$, $q^{*}(p)$ для каждой стороны (bid/ask).
3. Публикует `fob.venue.v1.VenueLiquidityCurve` в Kafka `venue.liquidity.fob`.
4. Опционально материализует `SyntheticFlowOrder` (Kafka `venue.synthetic` + PostgreSQL `synthetic_orders`).

## Режимы

| Level | Описание                                                                                   | Метрика контроля                          |
| ----- | ------------------------------------------------------------------------------------------ | ----------------------------------------- |
| L1    | Fast monotone approximation                                                                | —                                         |
| L2    | L1 + выпуклость + Moreau/Tikhonov регуляризация                                            | $\varepsilon_1$ (cost), $\varepsilon_2$ (monotonicity) |
| L3    | L2 + калибровка по реальным execution reports                                              | $\varepsilon_3$ (execution-based error)   |

Graceful degradation: `L3 → L2 → L1 → OFF` при ухудшении данных. Метрика `last_curve_degradation_reason` фиксирует причину.

L3 mix формулой $S_{L3} = (1-w)\cdot S_{\text{model}} + w\cdot S_{\text{execution}}$ — см. [liquidity_curve_producer.hpp](../../../cpp/venues/src/app/liquidity_curve_producer.hpp).

## DEX/AMM специфика

Для Uniswap v3 LOB виртуальный, строится из pool state:

- [amm_pool_extractor.cpp](../../../cpp/venues/src/domain/amm_pool_extractor.cpp): `sqrtPriceX96` + `tick` + `liquidity` → дискретные уровни.
- [amm_virtual_lob.cpp](../../../cpp/venues/src/domain/amm_virtual_lob.cpp): virtual LOB.
- [amm_direct_fob.cpp](../../../cpp/venues/src/domain/amm_direct_fob.cpp): альтернативный путь — напрямую FOB-кривая из AMM-инварианта (минуя virtual LOB).

## SyntheticFlowOrder

Производная виртуальная заявка от имени площадки. Lifecycle:

- `active` — построена и доступна matching;
- `expired` — TTL истёк (cleanup pending);
- `used` — matching уже использовал и не должен повторять.

Связь: `synthetic_orders.curveid → venue_liquidity_curves.curve_id`, `synthetic_orders.snapshotid → venue_snapshots.snapshot_id`.

## Конфигурация

| env                       | Default              | Назначение                                       |
| ------------------------- | -------------------- | ------------------------------------------------ |
| `VENUE_LIQUIDITY_TOPIC`   | `venue.liquidity.fob`| Kafka topic для VenueLiquidityCurve              |
| `VENUE_SYNTHETIC_TOPIC`   | `venue.synthetic`    | Kafka topic для SyntheticFlowOrder               |
| `LOB_TO_FOB_TAU_SEC`      | `5`                  | $\tau$ для $v = q/\tau$                          |
| `LOB_TO_FOB_DEFAULT_LEVEL`| `L2`                 | Глобальный default; override в `venue_config`    |
| `CURVE_LEVEL`             | `L3`                 | Текущий целевой режим                            |

## Backtest и replay

`schema_version`, `min_compatible_schema_version`, `producer_version` в proto — для F-15. Backtest parity:
[backtest_parity_check.cpp](../../../cpp/venues/src/app/backtest_parity_check.cpp), [backtest_synthetic_scenarios.cpp](../../../cpp/venues/src/app/backtest_synthetic_scenarios.cpp).

## Связанные фичи

- F-11 (primary).
- F-04 — потребитель кривых.
- F-09 — потребитель SyntheticFlowOrder.
- F-15 — backtest parity на schema_version.

## Participates In Features

- [F-11](../../02-system/features/F-11-external-venues-lob-to-fob/), [F-04](../../02-system/features/F-04-batch-clearing/), [F-09](../../02-system/features/F-09-batch-combo-orders/), [F-15](../../02-system/features/F-15-backtest-replay/)

## Participates In Use Cases

- [UC-F11-03](../../02-system/use-cases/UC-F11-03-build-liquidity-curve/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F11-03-build-curve-services](../sequences/SEQ-F11-03-build-curve-services.md)

## Produced Events

- [venue.liquidity.fob](../../06-api/messaging/venue-topics.md#venue-liquidity-fob)
- [venue.synthetic](../../06-api/messaging/venue-topics.md#venue-synthetic)

## Consumed Events

- (внутренний канал от Normalizer)

## Data Access

- [venue_liquidity_curves](../../07-data/venue-liquidity-curves.md) (ClickHouse, W — pending)
- [synthetic_orders](../../07-data/synthetic-orders.md) (PostgreSQL, W)
