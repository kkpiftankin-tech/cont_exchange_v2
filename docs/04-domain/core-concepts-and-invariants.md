---
id: DOC-DOMAIN-CORE-CONCEPTS
phase: 04-domain
status: active
owner: core-team
---

# Базовые понятия и инварианты FOB/CSLO

Перенесено из CLAUDE.md §8 (2026-09-28) при сокращении корневого файла — см.
[`.claude/rules/agent-routing.md`](../../.claude/rules/agent-routing.md) и
конспект в `incoming-docs/` про экономику контекста. Это определения базовых
терминов и MVP-инвариантов; технические имена полей — в
[`entities.md`](entities.md) и `contracts/proto/`, формулы клиринга/риска/hedge
— в [`business-rules.md`](business-rules.md), детальная модель IN-012 —
там же (`## Clearing Mechanics (IN-012)`).

## Базовые понятия

- `Asset` — торгуемый актив: BTC, ETH, USDT.
- `Instrument` / `Symbol` — торговая пара, например BTC/USDT.
- `Base asset` — первый актив пары, объём сделки выражается в нём.
- `Quote asset` — второй актив пары, цена выражается в нём.
- `Price` — количество quote asset за единицу base asset.
- `Side` — buy или sell.
- `Trading speed` — поток объёма в единицу времени, в base units/sec.
- `Execution window` — интервал исполнения $[t_{\text{start}}, t_{\text{end}}]$.

## FlowOrder

`FlowOrder` — центральная бизнес-сущность MVP. Технические имена полей —
proto-контракт `contracts/proto/fob/orders/v1/orders.proto` (источник истины
по приоритету CLAUDE.md §3.1): `order_id`, `client_order_id`, `user_id`,
`account_id`, `instrument`, `side`, `total_qty`, `remaining_qty`, `price_low`,
`price_high`, `max_speed`, `status`, `tif`, `tags`.

Инварианты:

1. `total_qty > 0`.
2. `remaining_qty >= 0`.
3. `remaining_qty <= total_qty`.
4. `price_low > 0`.
5. `price_high > 0`.
6. `price_low <= price_high`.
7. `max_speed > 0`.
8. `instrument.symbol`, `instrument.base`, `instrument.quote` не пустые.
9. Для `BUY` клиент платит quote и получает base.
10. Для `SELL` клиент отдаёт base и получает quote.
11. Любая команда create/amend/cancel должна быть идемпотентной.
12. Любая команда должна иметь `EventMeta.correlation_id`.

См. Conflict Note в [`entities.md`](entities.md#flowoder-бизнес-форма-in-001-8)
о более старой бизнес-форме поля (IN-001), которая этим именам не совпадает.

## CSLO

Continuous Scaled Limit Order — кривая спроса/предложения «цена–скорость»
или «цена–объём».

Типичная параметризация:

- $P_L$ — нижняя граница цены;
- $P_H$ — верхняя граница цены;
- $Q$ — максимальный объём;
- $U$ — максимальная скорость.

FlowOrder в текущем MVP — минимальная DTO-форма, совместимая с CSLO/FOB
моделью. Полная модель непрерывного рынка (N-агентная агрегация, векторное
представление, IN-012) — `business-rules.md`, раздел `## Clearing Mechanics (IN-012)`.

## BatchResult

`BatchResult` — результат одного clearing cycle.

Содержит:

- `batch_id`;
- timestamp;
- `clear_prices`: symbol → clearing price;
- `executed_rates`: order_id → executed rate;
- `fills`: список `FlowFill`;
- `order_updates`;
- `diagnostics`: residual norm, solve time, active orders, solver config version.

Инварианты:

1. Fill не должен превышать `remaining_qty` заявки.
2. Цена fill должна быть в допустимом диапазоне заявки, кроме явно описанных
   exceptional policies.
3. `batch_id` должен связывать fills, order updates, risk events и agent logs.
4. Любая ошибка solver должна давать диагностируемый результат, а не молчаливое
   повреждение состояния.

## VWAP и Implementation Shortfall

Операционные формулы VWAP/IS, используемые в backtest-отчётности, — в
[`business-rules.md`](business-rules.md#f-15--backtest--replay) (F-15 раздел,
`### VWAP`, `### Implementation Shortfall`). Определение здесь — только
концептуальное: VWAP — средневзвешенная по объёму цена исполнения; IS —
разница между идеальной стоимостью исполнения по цене принятия решения и
фактической стоимостью с учётом spread, temporary impact, permanent impact,
fees и volatility.
