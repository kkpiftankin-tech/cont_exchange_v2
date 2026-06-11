<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F11-03. Построить VenueLiquidityCurve (LOB → FOB)

## Feature

- [F-11. External Venues / LOB → FOB](../../features/F-11-external-venues-lob-to-fob/)

## Primary Actor

System (Venue Liquidity Curve Builder).

## Supporting Actors

- Venue Market Data Normalizer — поставщик VenueSnapshot.
- Matching / Execution Planning (F-04, F-12) — потребители кривых.
- ClickHouse `venue_liquidity_curves` — приёмник истории.

## Preconditions

- Получен `VenueSnapshot` со `status="connected"`.
- Заявка не stale: `now - timestamp <= stale_threshold_ms`.
- `venue_config.curve_level` (`L1`/`L2`/`L3`) задан.

## Trigger

Новое сообщение в Kafka `venue.snapshots` или прямой in-process вызов (текущий MVP — последний вариант).

## Main Flow

1. Curve Builder получает `VenueSnapshot`.
2. Builder вычисляет $p(q)$ — кумулятивную функцию предельной цены исполнения по уровням стакана.
3. Builder вычисляет $S(q) = \int_{0}^{q} p(x)\,dx$ — интегральную стоимость.
4. По временному масштабу $\tau$ (из env `LOB_TO_FOB_TAU_SEC` или `tau_ms` в payload) строится FOB-кривая по скорости:

   $$
   L(v) = \frac{S(v\tau)}{\tau}
   $$

5. В режиме L2 добавляется регуляризация (Moreau/Tikhonov) с контролем `epsilon1` (cost) и `epsilon2` (monotonicity).
6. В режиме L3 поверх L2 применяется калибровка по execution reports; обновляется `epsilon3`.
7. Builder заполняет дуальный слой $S^{*}(p)$, $q^{*}(p)$ и `confidence`.
8. Builder формирует `fob.venue.v1.VenueLiquidityCurve` со `schema_version`, `producer_version`.
9. Builder публикует в Kafka `venue.liquidity.fob`.
10. ClickHouse `venue_liquidity_curves` принимает запись (через ingestion-слой; pending T-F11-110).
11. Если `venue_config.synthetic_enabled=true`, Builder материализует `SyntheticFlowOrder` (lifecycle `active`) и публикует в Kafka `venue.synthetic` + PostgreSQL `synthetic_orders`.

## Alternative Flows

### A1. Уровни недостаточны для построения кривой

1. Builder выставляет `confidence` низкое, публикует кривую с `level` сниженным (например, было L2 — стало L1).
2. Метрика `last_curve_degradation_reason = "insufficient_depth"`.

### A2. Уровни stale

1. Builder отказывается публиковать новую кривую.
2. Если включён `publish_stale_l1_fallback=true`, публикует last-known L1 со снижением `confidence` и `level=L1`.

### A3. L3 нет калибровочных данных

1. Builder работает в режиме L2 (graceful degradation L3 → L2).
2. `last_curve_effective_level = "L2"`, `last_curve_degradation_reason = "no_calibration_history"`.

## Postconditions

- В Kafka `venue.liquidity.fob` опубликована свежая кривая.
- Метрики `last_curve_build_latency_ms`, `last_curve_confidence`, `last_curve_effective_level` обновлены.
- При `synthetic_enabled=true`: запись в `synthetic_orders` со `status=active`.

## Postconditions (negative)

- При фатальной ошибке (`bad input`, исключение в solver) Builder не публикует кривую; метрика `venue.empty_snapshots` или `last_curve_quality_action="dropped"`.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F11-03-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F11-03-build-curve-services.md)

## Related Contracts

- [venue-topics.md → venue.liquidity.fob](../../../06-api/messaging/venue-topics.md#venue-liquidity-fob)
- [venue-topics.md → venue.synthetic](../../../06-api/messaging/venue-topics.md#venue-synthetic)

## Related Components

- [venue-liquidity-curve-builder](../../../05-components/venue-liquidity-curve-builder/overview.md)

## Related Data

- [venue_liquidity_curves](../../../07-data/venue-liquidity-curves.md) (ClickHouse)
- [synthetic_orders](../../../07-data/synthetic-orders.md) (PostgreSQL)

## Source Fragments

- IN-004 §«LOB-to-FOB цепочка преобразования»
- IN-004 §«Режимы LOB-FOB» (Level 1/2/3)
- IN-004 §«Функциональные требования» F11-5..F11-11
