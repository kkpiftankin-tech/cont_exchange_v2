# Kafka Topic: positions.update

## Purpose

Лёгкий **invalidation-сигнал** «позиции пользователя изменились». ledger публикует
сообщение после успешного применения батча (`ApplyBatchResult` → `COMMIT` по
`accounts`/`positions`), gateway (ws-gateway) реагрегирует полный снимок позиций/маржи
и пушит его подписанному клиенту по WebSocket (F6-5).

Сигнал намеренно **не несёт состояния** (не содержит самих позиций/балансов): это
команда «перечитай», а не данные. См. [ADR-046](../../03-architecture/adr/ADR-046-positions-update-topic.md).

## Producer

- [ledger](../../05-components/ledger/overview.md) — после `ApplyBatchResult`

## Consumers

- [gateway](../../05-components/gateway/overview.md) (ws-gateway) — реагрегирует снимок
  через `positions_handler` (`GetPositions` ledger + `GetRiskSnapshot` risk) и пушит по WS

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 7 дней (целевая; транзиентный сигнал, история не требуется) |
| Partition key | `user_id` |
| Delivery | at-least-once, idempotent consumer (по `batch_id`) |
| Schema | `{ user_id, batch_id, ts }` (см. ниже) |

## Message schema

Лёгкий JSON/Protobuf-сигнал (контракт фиксируется при имплементации):

| Поле | Тип | Назначение |
| --- | --- | --- |
| `user_id` | string | пользователь, чьи позиции изменились; одновременно partition key |
| `batch_id` | string | батч, вызвавший изменение; ключ идемпотентности у потребителя |
| `ts` | timestamp | время эмиссии сигнала (после `COMMIT`) |

На один батч ledger эмиттит по одному сообщению на каждого затронутого `user_id`
(пользователи внутри батча дедуплицируются).

## Idempotency / delivery

- **At-least-once** ([ADR-020](../../03-architecture/adr/ADR-020-event-ordering-idempotency.md)):
  сигнал может продублироваться. Потребитель **идемпотентен по `batch_id`** —
  повторная реагрегация того же снимка безвредна; уже обработанный `batch_id`
  можно пропустить.
- Потеря/дубль сигнала приводит максимум к лишнему или пропущенному перечитыванию
  снимка, а не к рассинхрону данных (состояния в топике нет).
- Деградация: при недоступности WS клиент переходит на polling `GET /v1/positions`
  (F6-9) — push поверх `positions.update` не является единственным источником снимка.

## Registration (create_topics.sh)

Регистрацию в `infra/kafka/create_topics.sh` выполняет **кодовый агент** (не этот
docs-PR). Требуемая строка (retention 7 дней = `604800000` мс, key = `user_id`):

```sh
create_topic positions.update "$(retention_for_topic positions.update 604800000)" # key = user_id (positions invalidation signal; ledger -> ws-gateway)
```

После регистрации также добавить строку в таблицу «Core flow» в
[topics.md](topics.md).

## Related

- ADR: [ADR-046](../../03-architecture/adr/ADR-046-positions-update-topic.md)
- Feature: [F-06 Positions / PnL / Margin](../../02-system/features/F-06-positions-pnl-margin/feature.yaml) (F6-5)
- Related topics: [batch.outputs](batch-outputs.md), [risk.alerts](risk-alerts.md)
- Contracts (реагрегация снимка): `fob.ledger.v1.LedgerService.GetPositions`,
  `fob.risk.v1.RiskService.GetRiskSnapshot`
