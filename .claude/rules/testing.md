---
paths: ["**/tests/**", "Testing/**", "docs/10-testing/**", "docs/09-testing/**", "cpp/observability/**"]
---

# Observability и тестирование (перенесено из CLAUDE.md §19, §20)

## Observability rules

Для каждого нового production flow должны быть:

- correlation id;
- structured logs;
- metrics;
- error codes;
- alert thresholds, если есть риск деградации;
- dashboard/runbook reference.

Минимальные метрики:

- request latency;
- gRPC error rate;
- Kafka produce/consume failures;
- consumer lag;
- batch solve time;
- residual norm;
- number of active orders;
- fill rate;
- rejected/throttled orders;
- ledger apply failures;
- external venue rejection rate.

## Testing policy

Минимальные уровни тестирования:

- Unit tests для domain/app logic.
- Contract tests для `.proto` compatibility.
- Integration tests для gRPC service interactions.
- Kafka integration tests для producer/consumer flows.
- E2E tests для ключевых фич.
- Replay tests для deterministic matching.
- Performance tests для solver and critical endpoints.

### Основные E2E-фичи

Покрывай фичи:

- F-01 registration/auth — пока в новом skeleton отсутствует полноценная
  реализация;
- F-02 create FlowOrder;
- F-03 amend/cancel FlowOrder;
- F-04 batch clearing;
- F-05 live market data;
- F-06 positions/PnL/margin;
- F-07 pre-trade risk;
- F-08 post-trade risk/liquidations;
- F-09 portfolio/pair order;
- F-10 market-maker curves;
- F-11 external market data;
- F-12 execution hedge;
- F-13 post-trade report;
- F-14 deposit/withdraw;
- F-15 backtest/replay;
- F-16 operator panel/kill-switch;
- F-17 observability.

Если feature ещё не реализована, документируй статус как `planned` или
`stub` (в `feature.yaml` и/или в карточке `CALC-*`, если у неё уже есть
величина с этим статусом).

## Минимальные тестовые сценарии для solver (CLAUDE.md §15)

- один buyer и один seller с пересекающимися диапазонами;
- непересекающиеся диапазоны;
- частичное исполнение из-за speed cap;
- частичное исполнение из-за remaining_qty;
- несколько покупателей/продавцов;
- крайние цены;
- отмена заявки перед batch;
- replay того же batch input даёт тот же `BatchResult`.
