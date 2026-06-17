---
id: ADR-043
title: Combo external-нога → HedgeFlow (на ногу) + ChildOrder (на chunk) — интеграция F-09↔F-12
status: accepted
date: 2026-06-17
level: sea
feature: F-09
related: [ADR-037]
---

# ADR-043 — Combo external execution через HedgeFlow + ChildOrder (F-09 ↔ F-12)

## Контекст

Combo external-нога (venue ≠ internal) исполняется потоком, дроблёным по `qRate`
(ADR-037 + модель continuous_order_market): matching за раунд шлёт chunk
`min(remaining, qRate)` как `ExecutionIntent` (in-flight guard от over-fill).

venues (F-12) уже материализует на каждый `ExecutionIntent`:
- `HedgeFlow` (`hedgeflow_repo_->InsertOpen(intent)`),
- `ChildOrder` (`child_order_repo_->InsertPending(intent)`),
- и `ApplyReport` по `ExecutionReport`.

**Проблема (до этого ADR):**
- `child_order_id = intent.client_order_id()`, а matching ставил
  `client_order_id = leg_id` (константа по ноге) → все chunk'и одной ноги дают
  ОДИН `child_order_id` → `ON CONFLICT` → в `child_orders` 1 строка на ногу
  вместо строки на каждый chunk.
- `hedge_flow_id = intent.intent_id()` (uuid на chunk) → НОВЫЙ HedgeFlow на каждый
  chunk, без группировки.

Итог: дробление по qRate работает (filled_cum растёт постепенно), но в `child_orders`
/`hedgeflows` это НЕ отражено корректно (нет «1 HedgeFlow на ногу + N ChildOrders»).

## Решение

Combo external-нога ↔ ОДИН HedgeFlow; каждый qRate-chunk ↔ ОТДЕЛЬНЫЙ ChildOrder.

В `BuildExternalIntent` (matching):
- `intent.hedge_flow_id = leg_id` — все chunk'и ноги группируются под одним
  HedgeFlow (venues: `hedge_flow_id` берётся из intent).
- `intent.client_order_id = "{leg_id}#{intent_id}"` — уникален на chunk → venues
  делает отдельный `ChildOrder` на каждый chunk (`child_order_id` = client_order_id).
- `intent.internal_order_id = leg_id` (как было) — внутренняя ссылка.

Линковка отчёта обратно на combo-ногу: matching парсит `leg_id` из префикса
`client_order_id` до `#` (раньше использовал весь `client_order_id`). Report эхо-
копирует `client_order_id`, поэтому `parent = FindComboLegParent(leg_id_prefix)`.

venues НЕ меняется — он уже выводит `child_order_id` из `client_order_id` и
`hedge_flow_id` из `intent.hedge_flow_id`.

Трассировка: `combo_order_legs.leg_id` == `hedgeflows.hedge_flow_id`;
`child_orders.hedge_flow_id` → тот же; `child_orders.client_order_id = leg_id#chunk`;
`intent.reason = "combo_external_leg:{parent_order_id}"` хранит combo-родителя.

## Альтернативы

- **Report несёт и child_order_id, и leg-ссылку отдельными полями** — чище, но
  требует менять контракт `ExecutionReport` и venues-эхо. Отложено (structured
  `client_order_id` достаточно и не ломает контракт).
- **Оставить throttle только на уровне intent, без ChildOrder-на-chunk** — теряется
  F-12 трассировка/аналитика по срезам (child_orders).

## Последствия

- ✅ `child_orders` отражает реальные срезы (N строк на ногу), сгруппированы под
  одним `hedge_flow` на ногу → корректная F-12 аналитика и аудит.
- ✅ Контракты `ExecutionIntent`/`ExecutionReport` не меняются (используем
  существующие поля hedge_flow_id / client_order_id).
- ✅ venues не трогаем.
- ⚠️ `client_order_id` теперь структурный (`leg_id#chunk`) — любой потребитель,
  парсящий его как «чистый leg_id», должен брать префикс до `#` (обновлён matching).
- ⚠️ Нарезка пока по qRate (один размер chunk = qRate); учёт lot size / venue
  microstructure (разные размеры/частоты child-ордеров) — следующий шаг.

## Обратимость

Высокая. Откат: вернуть `client_order_id=leg_id` и убрать `hedge_flow_id` в
BuildExternalIntent + парсинг префикса. Данные `child_orders`/`hedgeflows` —
аналитические, не источник истины для combo-fill (он в `combo_order_legs`).

## Трассировка

- Feature: F-09; связано с F-12 (HedgeFlow/ChildOrder).
- Code: `cpp/matching/src/app/combo_external_routing.cpp` (BuildExternalIntent),
  `cpp/matching/src/app/matching_loop.cpp` (on_external_execution_report),
  venues `postgres_child_order_repository.cpp` / `postgres_hedgeflow_repository.cpp`.
- Data: `docs/07-data/child-orders.md`, `docs/07-data/hedgeflows.md`.
- Related: [ADR-037](ADR-037-... ), модель continuous_order_market (qRate flow limit).
