---
id: ADR-024
status: proposed
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - docs/03-architecture/adr/ADR-023-lob-to-fob-curve.md
  - docs/03-architecture/adr/ADR-027-execution-routing-algorithm.md
  - docs/02-system/features/F-11-external-venues-lob-to-fob/
---

# ADR-024: Time Alignment & Latency-Aware Venue Curve Correction

## Контекст

Внешняя venue-ликвидность ([ADR-023](ADR-023-lob-to-fob-curve.md)) наблюдается с
задержкой: к моменту фактического прибытия нашего child-order на venue стакан
уже изменился. Routing/хедж по «старой» кривой систематически недооценивает
slippage. Нужна коррекция кривой на горизонт задержки. Это **forward-looking**
решение — на момент написания не реализовано; фиксируем как `proposed`.

## Решение (proposed)

Ввести latency-aware коррекцию venue-кривой перед использованием в routing:

- **LatencyForecast**: оценка `arrivalHorizonMs` до venue (из `venue.health` /
  исторических замеров).
- **TimeAlignedVenueCurve**: прогноз состояния venue на момент прибытия —
  кривая корректируется на:
  - **depth decay** (ожидаемое «съедание»/обновление глубины);
  - **volatility penalty** (рост неопределённости цены с горизонтом);
  - **uncertainty penalty** (доверие к прогнозу убывает с задержкой);
  - **health penalty** (деградация/инстабильность venue).
- Результат подаётся в Execution Planning
  ([ADR-027](ADR-027-execution-routing-algorithm.md)) вместо «сырой» кривой.

## Альтернативы

- **Игнорировать задержку** (использовать наблюдаемую кривую как есть) —
  текущее поведение MVP; систематически недооценивает slippage.
- **Статический latency-буфер** (фиксированная надбавка к slippage) —
  проще, но не учитывает волатильность/health.

## Последствия

- **Плюс:** более точная оценка стоимости хеджа, меньше отрицательных сюрпризов по slippage.
- **Минус:** модель прогноза сложна, нуждается в калибровке и replay-валидации; риск переусложнения MVP.

## Обратимость

Высокая на этапе proposed. После внедрения в routing — средняя (нужна replay parity при изменении модели коррекции).

## Триггер для перевода в accepted

Реальные данные о slippage-ошибке из-за latency (после запуска F-12 хеджа) и/или
требование SLA по качеству исполнения, которое не достигается без коррекции.
