---
id: ADR-021
status: accepted
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - contracts/proto/fob/orders/v1/orders.proto
  - docs/04-domain/entities.md
  - docs/04-domain/business-rules.md
  - CLAUDE.md (§8.2 FlowOrder)
---

# ADR-021: FlowOrder — доменная модель и жизненный цикл

## Контекст

`FlowOrder` — центральная бизнес-сущность MVP (CLAUDE.md §8.2), описана в
`contracts/proto/fob/orders/v1/orders.proto` и в `docs/04-domain/entities.md`.
Инварианты и параметризация уже зафиксированы, но единого ADR по жизненному
циклу и инвариантам нет — это нужно для matching, risk и ledger, которые
опираются на статусы и remaining_qty.

## Решение

Зафиксировать модель и жизненный цикл FlowOrder как доменный канон.

### Параметризация (минимальная)

`order_id`, `client_order_id` (idempotency), `user_id`/`account_id`,
`instrument` (symbol/base/quote), `side`, `total_qty`, `remaining_qty`,
`price_low`, `price_high`, `max_speed`, `status`, `tif`, `tags`.

### Жизненный цикл

```text
new → risk_pending → active → partially_filled → filled
active → cancelled
active → expired
active → liquidated
```

### Инварианты

- `total_qty > 0`, `0 <= remaining_qty <= total_qty`.
- `price_low > 0`, `price_high > 0`, `price_low <= price_high`.
- `max_speed > 0`; `instrument.{symbol,base,quote}` непусты.
- `remaining_qty = max(total_qty - filled_cum, 0)`; `filled_cum >= total_qty ⇒ status = filled`.
- BUY: платит quote, получает base. SELL: отдаёт base, получает quote.
- Любая команда create/amend/cancel идемпотентна и несёт `EventMeta.correlation_id`.

## Альтернативы

- **Дискретный лимитный ордер вместо потокового** — отклонено: противоречит доменной идее FOB/CSLO (поток объёма с конечной скоростью).
- **Без явного `max_speed`** — отклонено: скорость исполнения — ключевой параметр CSLO-кривой.

## Последствия

- **Плюс:** matching/risk/ledger опираются на единый контракт статусов и инвариантов.
- **Минус:** инварианты нужно проверять на каждом переходе (валидация на границах).

## Обратимость

Низкая. FlowOrder — ядро домена; изменение модели затрагивает все сервисы и proto-контракт.
