---
id: ADR-025
status: accepted
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - contracts/proto/fob/risk/v1/risk.proto
  - docs/05-components/risk-manager/overview.md
  - docs/04-domain/business-rules.md
  - CLAUDE.md (§16 risk rules)
---

# ADR-025: Risk Manager — границы и pre-trade / pre-hedge / post-trade split

## Контекст

Есть ADR по docs/proto/venue/data, но нет ADR, фиксирующего **границы
ответственности Risk Manager**. Risk уже реализует pre-trade checks (F-07) и
`PreHedgeCheck` (F-12), kill-switch и risk alerts (CLAUDE.md §16). Без явной
границы есть риск, что routing/matching/ledger-логика «протечёт» в risk.

## Решение

Зафиксировать зоны ответственности Risk Manager.

### Ответственность

- **pre-trade check** — до активации FlowOrder (notional, max position,
  leverage, order rate, whitelist).
- **pre-hedge check** (`RiskService.PreHedgeCheck`) — перед хеджем (provider
  halted, notional, exposure, slippage, venues availability).
- **post-trade check** — связан с `batch_id`.
- **margin status**, **risk events / alerts** (`risk.alerts`), **kill-switch**
  (global или instrument-specific, защищён авторизацией).
- Решение `RiskDecision` ∈ {ACCEPT, REJECT, RESIZE, HALT}.

### НЕ ответственность

- matching / clearing решения;
- venue routing optimization (это Execution Planning, [ADR-013](ADR-013-execution-planning-placement.md));
- реализация ledger-учёта (это Ledger, [ADR-026](ADR-026-ledger-accounting-pnl.md)) — risk **не** мутирует ledger напрямую, кроме явно описанных liquidation/rebalance flows.

## Альтернативы

- **Risk внутри matching/order-flow** — отклонено: нарушает SRP и CLAUDE.md §10.3 (границы сервисов).
- **Risk мутирует ledger напрямую** — отклонено: ledger — единственный source of truth балансов; risk влияет через события/intents.

## Последствия

- **Плюс:** чёткие границы; risk-политику можно менять, не трогая matching/ledger.
- **Минус:** дополнительные gRPC-вызовы (pre-trade/pre-hedge) на горячем пути.

## Обратимость

Средняя. Границы зафиксированы в контрактах и коде; изменение политики требует обновления docs (§16) + тестов на false positive/negative.
