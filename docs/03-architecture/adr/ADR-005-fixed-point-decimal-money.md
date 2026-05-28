---
id: ADR-005
status: accepted
date: 2026-05-13
owners:
  - architecture
  - core-team
related:
  - docs/03-architecture/adr/ADR-026-ledger-accounting-pnl.md
  - contracts/proto/fob/common/v1/common.proto
  - docs/04-domain/entities.md
  - CLAUDE.md (§9 финансовая точность)
---

# ADR-005: Fixed-point Decimal для денег, qty, price, fee, PnL

## Контекст

Непрерывная биржа работает с crypto/fiat парами, разными `tickSize` /
`lotSize`, и считает деньги, цены, количество, fee, PnL и margin. Двоичный
`double` накапливает ошибку представления (0.1 непредставима точно), что
недопустимо для ledger / risk / matching settlement: расхождение в младших
разрядах ломает резервы, расчёт маржи и реконсиляцию.

`contracts/proto/fob/common/v1/common.proto` уже определяет `Decimal`
(`value = units * 10^(-scale)`). CLAUDE.md §9 уже запрещает `double` для
денежных величин. Этот ADR фиксирует решение явно.

## Решение

- Использовать **fixed-point `Decimal`** (proto `common.v1.Decimal`,
  `units` + явный `scale`) для всех денежных величин в домене, ledger,
  risk и matching settlement, а также в persistence.
- `double` / `float` **запрещён** для денег в ledger / risk / matching
  settlement layer.
- `double` **допустим** только для: solver diagnostics, residual norm,
  metrics, и research/simulation вычислений, **результат которых не
  попадает в боевой ledger** (например, движок F-20 VenueSimulator считает
  impact/VWAP в double — это sim-телеметрия, не боевые деньги).

### Обязательные правила Decimal

- `scale` фиксируется явно; sign не теряется при конвертации.
- Определены rounding policy и overflow policy.
- Сериализация — через proto `Decimal` (units/scale), не через float.
- Сравнение Decimal — типобезопасное; нельзя смешивать base amount,
  quote amount и price без явных типов/имён.
- Округление покрыто unit-тестами.

## Альтернативы

- **`double` везде** — отклонено: ошибки представления, недопустимо для ledger.
- **Arbitrary-precision decimal везде** (boost::multiprecision) — отклонено для горячих путей: overkill и медленнее fixed-point int64-базиса.
- **int64 minor units per asset** — частично эквивалентно fixed-point; `Decimal(units, scale)` обобщает это и уже есть в proto.

## Последствия

### Положительные

- Точные деньги; отсутствие float-drift в ledger/risk/settlement.
- Единый контрактный тип `Decimal` между сервисами.

### Отрицательные

- Больше кода для арифметики/конвертации, чем с `double`.
- Нужно дисциплинированно держать границу «double только для не-ledger симуляций».

## Обратимость

Низкая. Замена денежного типа затронула бы ledger, risk, matching и proto-контракты.
