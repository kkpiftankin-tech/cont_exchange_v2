---
paths: ["contracts/**", "infra/kafka/**", "docs/06-api/**"]
---

# Контракты и топики (перенесено из CLAUDE.md §7, §11.3, §11.4, §13, §25.2, §25.3)

## Protobuf packages

Контракты хранятся в `contracts/proto/fob/**/v1/*.proto`. Они компилируются
через `contracts/CMakeLists.txt` в C++ library `contracts_proto`.

Не меняй generated files вручную (`.pb.cc`, `.pb.h`, `.grpc.pb.cc`,
`.grpc.pb.h`, CLAUDE.md §12.4). Меняй `.proto`, затем пересобирай проект.
Если generated files появляются в build directory, не добавлять их как
source-of-truth.

## Основные protobuf-сущности

| Сущность | Файл | Назначение |
|---|---|---|
| `EventMeta` | `common.proto` | event id, timestamp, source, correlation id, trace/span id, partition key, tags |
| `Decimal` | `common.proto` | fixed-point decimal: `value = units * 10^(-scale)` |
| `FlowOrder` | `orders.proto` | потоковая заявка с диапазоном цены и max speed |
| `OrdersNormalized` | `orders.proto` | Kafka envelope для create/cancel/amend |
| `BatchResult` | `batch.proto` | результат clearing cycle: prices, rates, fills, order updates, diagnostics |
| `RiskAlert` | `risk.proto` | событие риска / kill-switch / margin / limit breach |
| `ExecutionIntent` | `execution.proto` | намерение внешнего хеджирования |
| `ExecutionReport` | `execution.proto` | отчёт внешней площадки |
| `AgentLog` | `agent.proto` | state–action–reward log для политик matching/risk/execution |

## Kafka / Redpanda topics

Dev topics создаются в `infra/kafka/create_topics.sh`:

| Topic | Producer | Consumer | Назначение |
|---|---|---|---|
| `marketdata.raw` | `venues` | `market_data`, потенциально `matching` | сырые внешние котировки / стаканы / trades |
| `orders.normalized` | `order_flow` | `matching` | нормализованные FlowOrder commands |
| `batch.outputs` | `matching` | `ledger`, `order_flow`, `market_data`, `backtest`, `risk`, `observability`, UI stream | clearing result, fills, order updates |
| `fills` | `matching` | (no live consumer; reserved for downstream analytics) | per-fill events extracted from `batch.outputs` |
| `venue.snapshots` | `venues` | `market_data`, `backtest` | normalized venue LOB snapshots |
| `venue.liquidity.fob` | `venues` | `matching`, `market_data`, `backtest` | venue LOB→FOB liquidity curves (F-11) |
| `venue.health` | `venues` | `matching`, `risk`, `observability` | venue health + routing recommendations (F-11) |
| `execution.intents` | `risk`/`matching`/future execution policy | `venues`, `ledger` | child order / hedge instruction |
| `execution.venue` | `venues` | `ledger`, `market_data`, `risk` | venue execution reports (canonical post-IN-005 name) |
| `execution.reports` | `venues` (legacy mirror of `execution.venue`) | `ledger` | backwards-compatible alias kept during migration — IN-005 / ADR-pending |
| `backtest.execution.venue` | `venues` (replay mode only) | `backtest` | F-12 backtest execution reports, isolated from live history |
| `risk.alerts` | `risk` | `observability`, operator UI | alert stream |
| `agent.logs` | будущие agent policies | `backtest`, `research`, `observability` | state–action–reward trail |

Note on `execution.venue` vs `execution.reports`: `venues` publishes each
ExecutionReport to BOTH topics (see `cpp/venues/src/infra/execution_report_producer.cpp`).
`ledger` consumes both in one consumer group with idempotency on `report_id`,
so the dual publish is safe and the legacy topic can be retired later without
breaking running ledger instances.

При добавлении нового топика обнови (навык `register-kafka-topic` покрывает
шаги 1, 3):

1. `docs/06-api/messaging/topics.md`;
2. schema/proto file;
3. `infra/kafka/create_topics.sh`;
4. producer/consumer code;
5. integration tests;
6. observability docs.

## Правила Kafka/Redpanda

- Kafka record key должен быть выбран осмысленно: `user_id`, `symbol`,
  `order_id`, `batch_id`, `intent_id`.
- Для financial event stream предпочитай at-least-once + idempotent consumer.
- Consumer должен коммитить offset после успешной обработки.
- Нельзя полагаться на exactly-once без явного проектного решения и тестов.
- Replay должен быть возможен для audit/backtest.
- События не должны содержать secrets.
- Retention должен соответствовать назначению: market data может быть
  short-retention, audit/risk/fills — long-retention.

## Contract-first (при изменении API)

1. обнови `.proto` или OpenAPI/schema;
2. обнови документацию контракта (`docs/06-api/`);
3. обнови сервис-реализацию;
4. обнови клиента;
5. обнови тесты;
6. убедись, что старые consumers не ломаются без миграционного плана.

Для breaking changes нужен ADR (CLAUDE.md §3.3).

## Event-first (для Kafka-flow)

1. опиши событие и topic;
2. зафиксируй partition key;
3. зафиксируй delivery semantics;
4. продумай idempotency;
5. продумай replay;
6. добавь producer;
7. добавь consumer;
8. добавь observability;
9. добавь test/replay scenario.

## Чек-лист: добавить gRPC method

1. Обновить `.proto`.
2. Проверить backward compatibility.
3. Обновить `docs/06-api/grpc/`.
4. Пересобрать contracts.
5. Реализовать server method в `transport/`.
6. Добавить application use case.
7. Добавить client wrapper, если нужен.
8. Добавить tests.

## Чек-лист: добавить Kafka event

Совпадает с Event-first выше; навык `register-kafka-topic` автоматизирует
шаги 1–3 для НОВОГО топика (schema file + `create_topics.sh` + doc-stub).
