---
id: DOC-BUSINESS-VALUE
phase: 01-business
status: draft
owner: core-team
---

# Value Proposition

Что получает каждый сегмент пользователей.

## Liquidity Trader

- Контролируемый профиль исполнения во времени.
- Прогноз IS с доверительным интервалом до отправки.
- Защита от front-running.

## Market Maker / LP

- Экономически устойчивая среда: компенсация inventory-риска в slope CSLO.
- Меньше токсичного потока по сравнению с CLOB.

## Client Integrator / Broker

- Стандартизованный контракт на flow orders.
- Аудируемая отчётность по IS, VWAP, fill rate.
- Готовые алгоритмы (TWAP/VWAP/optimal) поверх FOB.

## Operator

- Полный обзор состояния платформы, kill-switch.
- Регуляторная отчётность из ClickHouse и risk_events.

См. также [stakeholders.md](stakeholders.md).
