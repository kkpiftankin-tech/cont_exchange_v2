---
id: DOC-BUSINESS-VISION
phase: 01-business
status: draft
owner: core-team
related:
  - docs/01-business/stakeholders.md
  - docs/01-business/value-proposition.md
  - incoming-docs/EXPLANATORY-NOTE-source.md
---

# Vision

## Назначение

Описать, зачем существует Continuous Exchange и какую боль она решает.

## Контекст

Классические биржи работают как Limit Order Book (LOB) с дискретными ордерами; маркет-мейкеры
вынуждены защищаться от токсичного потока высокочастотных трейдеров, а ликвидные трейдеры
платят высокий impact на крупных сделках. AMM на DEX подвержены MEV и front-running.

## Основное содержание

Continuous Exchange реализует **flow trading**: трейдер задаёт не точку (цену и объём),
а **поток во времени** — диапазон цен `[p_L, p_H]`, скорость `q` и максимальный объём `Q_max`.
Биржа периодически решает задачу клиринга, находя единые цены и скорости исполнения,
сбалансированные по совокупному спросу и предложению.

Это даёт:

- **снижение Implementation Shortfall** для крупных и средних сделок ликвидных трейдеров;
- **экономически устойчивый market making**: компенсация инвентори-риска заложена в slope кривых;
- **прозрачность и предсказуемость** исполнения — заранее известны диапазоны цены и VWAP;
- **защиту от MEV и токсичного потока** за счёт дизайна механизма с штрафом за высокую скорость.

Концептуальная основа — модели Kyle-Lee (Continuous Scaled Limit Orders, 2017) и их реализация в Krypton.

## Инварианты / правила

1. Сделки реализуются как **непрерывный поток** во времени, а не как дискретные точки.
2. В каждый момент существует **единая клиринговая цена** на инструмент.
3. **Никаких скрытых преимуществ** отдельным клиентам: правила одинаковы для всех.
4. **Детерминированное исполнение**: одни и те же входные данные → один и тот же результат.

## Связанные артефакты

- [stakeholders.md](stakeholders.md) — для кого мы это строим
- [value-proposition.md](value-proposition.md) — что получает каждый сегмент
- [../04-domain/ubiquitous-language.md](../04-domain/ubiquitous-language.md) — CSLO, FOB, IS, VWAP
- [../03-architecture/architecture-overview.md](../03-architecture/architecture-overview.md) — как это устроено

## Открытые вопросы

- Регуляторный режим: spot crypto, derivatives, multi-asset?
- Конкуренция: с CEX (Binance) или с DEX-aggregator'ами (CoW Swap, 1inch)?

## История изменений

| Дата | Изменение |
| --- | --- |
| 2026-05-13 | Первая версия из incoming-docs/EXPLANATORY-NOTE-source.md |
