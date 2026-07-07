---
id: ADR-046
title: Топик positions.update для push-обновлений позиций (F6-5)
status: proposed
date: 2026-06-29
level: sea
feature: F-06
related: [ADR-003, ADR-020, ADR-042]
---

# ADR-046 — Kafka-топик `positions.update` для WS-push обновлений позиций

## Контекст

F6-5 требует WebSocket-push: после применения батча клиент должен получить
обновление своих позиций/маржи без перезапроса (T-F06-041). Для этого gateway
(ws-gateway) должен узнавать, **позиции какого пользователя** изменились в
результате батча, и отдавать ему свежий снимок.

Прямое потребление существующих топиков не подходит:

- `batch.outputs` несёт `BatchResult` + `FillEvent[]`, но **`FillEvent` не содержит
  `user_id`** — только `order_id`. gateway не может маршрутизировать push по
  пользователю, не зная маппинга `order_id → user_id`, а у gateway этого маппинга
  нет (он живёт в order_flow / ledger). Партиционирование `batch.outputs` — по
  `batch_id`, а push нужен по пользователю.
- `risk.alerts` появляется только при margin-call/liquidation, а push нужен на
  любое изменение позиции.

Нужен лёгкий, маршрутизируемый по пользователю сигнал «позиции пользователя
изменились батчем N».

## Решение

ledger публикует **новый Kafka-топик `positions.update`** после успешного
`ApplyBatchResult` (после `COMMIT` транзакции по `accounts`/`positions`):

- **key = `user_id`** — push маршрутизируется и партиционируется по пользователю
  (CLAUDE.md §13).
- **payload — лёгкий invalidation-сигнал**, а не полный снимок:
  `{ user_id, batch_id, ts }`. Это «позиции этого пользователя устарели, перечитай».
- gateway (ws-gateway) **консьюмит** `positions.update` и на каждый сигнал
  **реагрегирует полный снимок** через существующий `positions_handler`
  (`GetPositions` ledger + `GetRiskSnapshot` risk, как в `GET /v1/positions`,
  T-F06-040), затем пушит снимок подписанному клиенту.

ledger на один батч эмиттит по одному сообщению на каждого затронутого
`user_id` (дедуп пользователей внутри батча).

## Альтернативы

- **gateway консьюмит `batch.outputs` + lookup `order_id → user_id`.** Требует от
  gateway либо собственного индекса заказов, либо синхронного gRPC-lookup на каждый
  fill (нарушает границы сервисов — gateway не владеет этим маппингом). Отклонено:
  размазывает знание о пользователе по сервисам и создаёт горячий lookup на хвосте
  батча.
- **Расширить `FillEvent`/`batch.outputs` полем `user_id`.** Breaking-изменение
  контракта `BatchResult`, затрагивает matching, ledger, risk, backtest,
  observability и его replay-parity (CLAUDE.md §15, [ADR-042](ADR-042-unified-batch-read-layer.md)).
  Несоразмерно задаче UI-инвалидации. Отклонено.
- **Сигнал публикует risk** (после `RiskSnapshot`). risk уже знает затронутых
  пользователей. Но обновление позиций/балансов первично делает ledger; привязка
  push к risk теряет апдейты, где маржа не изменилась, и навязывает порядок
  ledger→risk. Отклонено в пользу ledger как owner'а позиций.

## Последствия

- ✅ gateway маршрутизирует push по `user_id` без знания внутренней структуры
  `BatchResult` и без `order_id → user_id` lookup.
- ✅ Контракты `BatchResult`/`FillEvent` не меняются.
- ✅ Полный снимок собирается существующим путём `positions_handler` → один
  источник правды для REST `GET /v1/positions` и WS-push (нет дублирования логики
  расчёта PnL/маржи).
- ⚠️ Дополнительный топик и producer в ledger; ещё один consumer-group в gateway.
- ⚠️ At-least-once ([ADR-020](ADR-020-event-ordering-idempotency.md)): сигнал может
  продублироваться. Безопасно — потребитель **идемпотентен по `batch_id`** (повторная
  реагрегация одного и того же снимка не вредит; устаревший `batch_id` можно
  пропускать). Сигнал намеренно не несёт состояния, поэтому потеря/дубль приводят
  максимум к лишнему/пропущенному перечитыванию, а не к рассинхрону данных.
- ⚠️ «Толстая» реагрегация на каждый сигнал — нагрузка на ledger/risk (gRPC),
  смягчается дедупом и пулом соединений ([ADR-045](ADR-045-pg-connection-pooling.md)).

## Обратимость

Высокая. Откат: убрать producer в ledger и consumer в gateway; топик удаляется.
Состояние нигде не персистится по этому топику (сигнал транзиентный), миграции
данных не требуется. Push можно временно деградировать до polling-fallback (F6-9).

## Трассировка

- Feature: [F-06](../../02-system/features/F-06-positions-pnl-margin/feature.yaml) (F6-5)
- Messaging: [positions.update](../../06-api/messaging/positions.update.md) (новый),
  [batch.outputs](../../06-api/messaging/batch-outputs.md), [topics.md](../../06-api/messaging/topics.md)
- Contracts: `fob.ledger.v1.LedgerService.GetPositions`,
  `fob.risk.v1.RiskService.GetRiskSnapshot` (реагрегация снимка)
- Code (целевые точки): ledger producer после `ApplyBatchResult`;
  gateway ws-gateway consumer + `positions_handler` (T-F06-041).
- Registration: строка `create_topic` в `infra/kafka/create_topics.sh` (ведёт
  кодовый агент — см. doc `positions.update.md`).
- Related: [ADR-003](ADR-003-kafka-redpanda-broker.md) (брокер),
  [ADR-020](ADR-020-event-ordering-idempotency.md) (idempotency / at-least-once).
