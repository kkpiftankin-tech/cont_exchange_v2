# Компонент: risk

Pre-trade и post-trade рисковая логика. Управляет kill switch (глобальный + по инструменту), публикует `risk.alerts`.

## Код

- [cpp/risk/](../../../cpp/risk/) — каталог
- [src/main.cpp](../../../cpp/risk/src/main.cpp) — entrypoint
- [src/app/risk_uc.cpp](../../../cpp/risk/src/app/risk_uc.cpp) — use-cases
- [src/transport/grpc_risk_service.cpp](../../../cpp/risk/src/transport/grpc_risk_service.cpp) — gRPC API
- [src/infra/risk_alerts_publisher.cpp](../../../cpp/risk/src/infra/risk_alerts_publisher.cpp) — Kafka producer

## gRPC API

`fob.risk.v1.RiskService`:

| RPC | Что делает |
|---|---|
| `CheckNewOrder` | Pre-trade проверка: ACCEPT / REJECT / HALT. Возвращает оценку начальной маржи |
| `SetKillSwitch` | Включить/выключить торги (глобально или по symbol). Эмиттит RiskAlert |
| `OnBatchResult` | Post-trade callback. В MVP только лог |

## Конфигурация

| env | Default | Назначение |
|---|---|---|
| `RISK_GRPC_LISTEN` | 0.0.0.0:50052 | Адрес gRPC сервера |
| `KAFKA_BROKERS` | redpanda:9092 | Kafka |

## Текущая логика проверок

[risk_uc.cpp:30-80](../../../cpp/risk/src/app/risk_uc.cpp#L30-L80):
1. Проверка kill-switch (глобальный / по symbol) → если halt, возвращаем `RISK_DECISION_HALT`.
2. `total_qty > 0` → иначе `BAD_QTY`.
3. `price_low <= price_high` → иначе `BAD_PRICE_RANGE`.
4. **Margin = qty * reference_price * 0.1** — placeholder формула, не учитывает диапазон цен.

## Известные ограничения

- **Нет учёта позиций пользователя** — каждая проверка stateless, без open positions/exposure.
- **Нет лимитов** — отсутствуют per-user, per-instrument лимиты.
- **State (kill_switch) только in-memory** — теряется при рестарте.

## Связанные фичи

- F-07 (Pre-trade Risk Control)
- F-12 (Execution Hedge) — TODO post-trade ветка

## Participates In Features

- [F-04](../../02-system/features/F-04-batch-clearing/), [F-06](../../02-system/features/F-06-positions-pnl-margin/), [F-07](../../02-system/features/F-07-pretrade-risk/), [F-08](../../02-system/features/F-08-posttrade-risk-and-liquidations/), [F-09](../../02-system/features/F-09-batch-combo-orders/), [F-10](../../02-system/features/F-10-mm-curves/), [F-12](../../02-system/features/F-12-execution-hedge/), [F-16](../../02-system/features/F-16-operator-console/), [F-17](../../02-system/features/F-17-monitoring-and-alerts/)

## Participates In Use Cases

- [UC-F04-01](../../02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md), [UC-F06-01](../../02-system/use-cases/UC-F06-01-show-positions/use-case.md), [UC-F07-01](../../02-system/use-cases/UC-F07-01-pretrade-risk-check/use-case.md), [UC-F08-01](../../02-system/use-cases/UC-F08-01-liquidate-position/use-case.md), [UC-F12-01](../../02-system/use-cases/UC-F12-01-execute-hedge/use-case.md), [UC-F16-01](../../02-system/use-cases/UC-F16-01-trigger-kill-switch/use-case.md), [UC-F17-01](../../02-system/use-cases/UC-F17-01-fire-alert/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F02-UC-F02-01-services](../sequences/SEQ-F02-UC-F02-01-services.md), [SEQ-F04-UC-F04-01-services](../sequences/SEQ-F04-UC-F04-01-services.md), [SEQ-F06-UC-F06-01-services](../sequences/SEQ-F06-UC-F06-01-services.md), [SEQ-F07-UC-F07-01-services](../sequences/SEQ-F07-UC-F07-01-services.md), [SEQ-F08-UC-F08-01-services](../sequences/SEQ-F08-UC-F08-01-services.md), [SEQ-F12-UC-F12-01-services](../sequences/SEQ-F12-UC-F12-01-services.md), [SEQ-F16-UC-F16-01-services](../sequences/SEQ-F16-UC-F16-01-services.md), [SEQ-F17-UC-F17-01-services](../sequences/SEQ-F17-UC-F17-01-services.md)

## Owned Contracts

- `fob.risk.v1.RiskService` — [../../06-api/grpc/](../../06-api/grpc/)

## Produced Events

- [risk.alerts](../../06-api/messaging/risk-alerts.md)
- (planned) [execution.intents](../../06-api/messaging/execution-intents.md)

## Consumed Events

- [batch.outputs](../../06-api/messaging/batch-outputs.md) — post-trade check

## Data Access

- (planned) `risk_limits`, `risk_snapshots`, `positions` — [../../07-data/data-overview.md](../../07-data/data-overview.md)
