# F-06 — Positions / PnL / Margin

## Описание

Учёт балансов, позиций, PnL и маржинальных требований пользователей. Бэкенд: `cpp/ledger` (балансы) + `cpp/risk` (margin TODO).

## Что реализовано (MVP)

- Балансы `available` / `reserved` по парам (user, currency)
- Резерв и release под ордера (идемпотентно)
- Применение fills из `batch.outputs`

## Что НЕ реализовано

- Positions (open/closed)
- PnL (realised/unrealised)
- Margin (формула в risk — placeholder = 10% notional)
- Persistence (in-memory)

## Файлы

- [cpp/ledger/src/app/ledger_uc.cpp](../../../../cpp/ledger/src/app/ledger_uc.cpp)
- [cpp/risk/src/app/risk_uc.cpp](../../../../cpp/risk/src/app/risk_uc.cpp) — margin placeholder

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна в любой момент показывать клиенту его позиции, прибыль/убыток (реализованный + нереализованный, mark-to-market) и доступный/используемый коллатерал.

- `positions` обновляются после каждого `BatchResult`.
- `accounts.free_balance` + `reserved_balance` + `venue_allocated` + `pending_transfer` показываются в UI.
- Margin утилизация и leverage пересчитываются Risk Manager.

Источник: IN-001 §6 FR-LEDGER-001/002.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
