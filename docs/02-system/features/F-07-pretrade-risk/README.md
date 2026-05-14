# F-07 — Pre-trade Risk Control

## Описание

Синхронный гейт перед резервом средств. Cинусоидальный вызов из order_flow в risk перед каждым CreateFlowOrder.

## Проверки (текущая реализация)

1. Kill-switch (глобальный / по symbol) → `HALT`
2. `total_qty > 0` → иначе `BAD_QTY`
3. `price_low <= price_high` → иначе `BAD_PRICE_RANGE`
4. Margin = `qty * reference_price * 0.1` (placeholder)

[cpp/risk/src/app/risk_uc.cpp](../../../../cpp/risk/src/app/risk_uc.cpp)

## Связанные

- F-02 (Create FlowOrder) — caller
- F-12 (Execution Hedge) — pre-hedge checks

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна предотвращать выставление заявок, нарушающих лимиты по `max_notional`, `max_position`, `max_leverage`, `max_order_rate`, `asset_whitelist`. Возможные решения: `ACCEPT` / `REJECT` / `THROTTLE` (с скорректированными `q_rate`/`q_max`) / `HALT` (kill_switch).

- Любая новая или amend заявка проходит проверку синхронно.
- При REJECT и THROTTLE событие пишется в `risk.alerts`.
- Reference price берётся из Market Data Service.

Источник: IN-001 §6 FR-RISK-001, §5.1 сценарии.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
