---
id: ADR-023
status: accepted
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-11-external-venues-lob-to-fob/
  - docs/06-api/messaging/venue-liquidity-fob.md
  - docs/03-architecture/adr/ADR-024-latency-aware-venue-curve.md
  - CLAUDE.md (§7.3 venue.liquidity.fob)
---

# ADR-023: LOB→FOB curve construction и validation

## Контекст

F-11 строит из внешних venue LOB-снапшотов (и AMM-состояния) непрерывные
кривые ликвидности «цена–объём/скорость» (`venue.liquidity.fob`), которые
потребляет matching и execution planning. Packaging этого пути зафиксирован в
[ADR-014](ADR-014-venues-binary-vs-components.md), но **математическая модель и
правила валидации** кривой нигде не закреплены как ADR, хотя
`venue-liquidity-curve-builder` уже реализован.

## Решение

Зафиксировать контракт и правила построения `VenueLiquidityCurve`:

### Вход / выход

- **Вход**: `VenueSnapshot` (LOB levels) или AMM-состояние.
- **Выход**: `VenueLiquidityCurve` — сетка `qGrid`, функция `pOfQ` (цена как
  функция объёма), `S(q)` (slippage), опционально `L(v)` (скорость),
  `confidence`, epsilon-параметры.

### Правила валидации

- **Монотонность**: `pOfQ` не убывает по объёму для buy-side (растёт стоимость
  с глубиной), симметрично для sell-side.
- **Convexity**: кумулятивная стоимость выпукла (нет «отрицательного impact»).
- **Stale rejection**: снапшот старше порога (`lob_age_ms`) отбраковывается.
- **Confidence threshold**: кривые ниже порога доверия не используются для routing.
- **Synthetic FlowOrder generation**: кривая может порождать синтетические
  FlowOrder для подачи в matching.
- **Time-alignment compatibility**: кривая совместима с latency-коррекцией
  ([ADR-024](ADR-024-latency-aware-venue-curve.md)).

Математика построения кривой использует `double` (не деньги ledger; см.
[ADR-005](ADR-005-fixed-point-decimal-money.md) — допустимая зона double).

## Альтернативы

- **Передавать сырой LOB в matching** — отклонено: matching не должен знать про venue-специфику; FOB-кривая — единый контракт.
- **Только AMM-формула без LOB** — отклонено: большинство venue дают LOB, нужна общая модель.

## Последствия

- **Плюс:** единый venue-agnostic контракт ликвидности для matching/routing.
- **Минус:** ошибки построения/валидации кривой напрямую влияют на качество routing и хеджа.

## Обратимость

Средняя. Контракт `venue.liquidity.fob` версионируется; смена математической модели обратима, пока выход совместим с consumers.
