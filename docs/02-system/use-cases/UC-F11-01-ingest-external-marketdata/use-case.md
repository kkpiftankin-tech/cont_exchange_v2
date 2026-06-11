<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F11-01. Принять external market data (legacy)

> **Статус:** **DEPRECATED — заменено на [UC-F11-02. Publish VenueSnapshot](../UC-F11-02-publish-snapshot/use-case.md).**
>
> Этот use case остался для совместимости старых ссылок (sequence-диаграмма, traceability в [F-11 README §«Use Cases»](../../features/F-11-external-venues-lob-to-fob/README.md#use-cases)). Использовать UC-F11-02 для всех новых ссылок и реализаций.
>
> Новый набор use case'ов F-11 живёт под номерами UC-F11-01..05:
>
> - [UC-F11-01. Onboard venue](../UC-F11-01-onboard-venue/use-case.md)
> - [UC-F11-02. Publish VenueSnapshot](../UC-F11-02-publish-snapshot/use-case.md) — пришёл на смену этой странице
> - [UC-F11-03. Build VenueLiquidityCurve](../UC-F11-03-build-liquidity-curve/use-case.md)
> - [UC-F11-04. Venue health degradation](../UC-F11-04-venue-health-degradation/use-case.md)
> - [UC-F11-05. Execute hedge on venue](../UC-F11-05-execute-hedge-on-venue/use-case.md)
>
> Текст ниже сохранён только как историческая запись.

## Feature

- [F-11. External Venues / LOB → FOB](../../features/F-11-external-venues-lob-to-fob/)

## Primary Actor

System (Venues adapter)

## Supporting Actors

- CEX / DEX (внешняя площадка)

## Preconditions

- Сконфигурирована площадка и инструмент.

## Trigger

Внешняя площадка шлёт стакан / тики / сделки.

## Main Flow (legacy, до IN-004)

1. Venues adapter получает LOB-снапшот.
2. Adapter преобразует LOB → CSLO кривую (`p_low`, `p_high`, `q_max`, `q_rate`).
3. Adapter публикует `marketdata.raw`.
4. Market Data Service обновляет кэш тикеров.
5. (Опционально) FOB-кривая включается в matching через `orders.normalized`.

## Replaced By

После IN-004 рендеринг переехал на отдельные топики:

- `venue.snapshots` — нормализованный VenueSnapshot (UC-F11-02);
- `venue.liquidity.fob` — построенная FOB-кривая (UC-F11-03);
- `venue.synthetic` — производная SyntheticFlowOrder (UC-F11-03).

`marketdata.raw` остаётся legacy-каналом для F-05 (см. Conflict Note C-2 в [README](../../features/F-11-external-venues-lob-to-fob/README.md#conflict-notes)).

## Related Sequence Diagrams

- [System sequence (legacy)](sequences/SEQ-UC-F11-01-system.md)
- [SEQ-F11-UC-F11-01-services (legacy)](../../../05-components/sequences/SEQ-F11-UC-F11-01-services.md)

## Related Contracts

- [marketdata.raw](../../../06-api/messaging/marketdata-raw.md) (legacy)
- [venue-topics.md](../../../06-api/messaging/venue-topics.md) (canonical)

## Related Components

- [external-venues-connector](../../../05-components/external-venues-connector/overview.md)
- [venue-market-data-normalizer](../../../05-components/venue-market-data-normalizer/overview.md)
- [market-data](../../../05-components/market-data/overview.md)
