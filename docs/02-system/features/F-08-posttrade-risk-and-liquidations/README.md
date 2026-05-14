# F-08 — Post-trade Risk Monitoring & Liquidations

> **Статус:** Not implemented.

Перерасчёт margin/VaR после каждого batch, margin calls, принудительные ликвидации. Зависит от F-06 (positions/PnL) и F-04 (источник batch-событий).

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна отслеживать изменение рисков после сделок, выявлять нехватку маржи, формировать margin call и принудительно ликвидировать позиции при необходимости.

- Триггер — каждое `batch.outputs`.
- При `maintenance_margin` breach генерируется `risk.alerts(MARGIN_CALL)`.
- Если в течение grace period маржа не восстановлена — создаётся ликвидационный `FlowOrder`.
- Все события и решения логируются в `risk_events`.

Источник: IN-001 §6 FR-RISK-002.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
