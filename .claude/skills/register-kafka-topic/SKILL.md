---
description: Register a new Kafka topic in cont_exchange_v2.0 — add `create_topic` line to `infra/kafka/create_topics.sh`, create documentation file in `docs/06-api/messaging/<topic>.md`, link it from related feature.yaml. Use when the user asks to add / create / register a new Kafka topic, event channel, or message stream.
---

# Skill: register-kafka-topic

## Purpose

Полное добавление нового Kafka-топика в проект:
1. Сам топик в `infra/kafka/create_topics.sh` (idempotent создание при старте кластера).
2. Документация в `docs/06-api/messaging/<topic>.md`.
3. Обновление `kafkaTopics.produces/consumes` в feature.yaml тех F-XX которые работают с топиком.

После выполнения skill'a следующий шаг — proto-contract-designer (если message type новый) или код producer/consumer.

## When to use

- "Добавь топик X"
- "Создай Kafka топик для F-XX"
- "Зарегистрируй <topic> в kafka"

## Required inputs

- **Topic name** (точное имя, `.`-разделённое: `<domain>.<purpose>`, например `reports.generated`)
- **Producer** — какой сервис публикует
- **Consumer(s)** — какие сервисы читают
- **Message type** — `fob.<package>.v1.<MessageName>` (proto должен существовать или быть запланирован)
- **Partition key** — поле для роутинга (`user_id` / `symbol` / `order_id` / `batch_id`)
- **Retention ms** — обычно `604800000` (7 дней) для finance, `86400000` (24ч) для market data
- **Linked features** — список F-XX к которым относится

## Step-by-step

### 1. Прочитать существующее
```bash
cat infra/kafka/create_topics.sh
ls docs/06-api/messaging/
```

### 2. Добавить в `infra/kafka/create_topics.sh`

Найти раздел `# === Application topics ===` и добавить **в алфавитном порядке**:

```bash
create_topic <topic_name> "$(retention_for_topic <topic_name> <retention_ms>)"
```

Пример:
```bash
create_topic reports.generated "$(retention_for_topic reports.generated 604800000)"
```

Также добавить функцию `retention_for_topic` если её ещё нет в скрипте (она генерирует config string для rpk).

### 3. Создать `docs/06-api/messaging/<topic>.md`

Шаблон (нормализованный по существующим примерам в этой папке):

```markdown
# Topic: <topic_name>

## Назначение

<1-2 параграфа: что несёт топик, в какой бизнес-цели участвует>

## Схема сообщения

**Message type:** `fob.<package>.v1.<MessageName>`
**Proto:** [contracts/proto/fob/<package>/v1/<file>.proto](../../../../contracts/proto/fob/<package>/v1/<file>.proto)

## Producer

- **<service>** — `cpp/<service>/src/infra/kafka/<topic>_producer.cpp`
- Когда публикует: <описание триггера>
- Поведение при ошибке: <retry / dead-letter / drop>

## Consumers

- **<service1>** — `cpp/<service1>/src/infra/kafka/<topic>_consumer.cpp`
  - Что делает с сообщением: <описание>
  - Idempotency: <по полю>
- **<service2>** — ...

## Параметры топика

| Свойство | Значение |
|---|---|
| Partitions | 3 (default из create_topics.sh) |
| Replication factor | 1 (dev) / 3 (prod) |
| Partition key | <field> |
| Delivery semantics | at-least-once + idempotent consumer |
| Retention | <ms> = <human-readable> |
| Создаётся | `infra/kafka/create_topics.sh` |

## Replay-семантика

<Можно ли реплеить? Что произойдёт у consumer'ов? Используется ли в F-15?>

## Связанные фичи

- [F-XX](../../02-system/features/F-XX-*/feature.yaml)

## Известные ограничения

<Если применимо: TTL trade-offs, schema evolution rules, ordering guarantees>
```

### 4. Обновить feature.yaml связанных F-XX

В каждом `docs/02-system/features/F-XX-*/feature.yaml`:

```yaml
kafkaTopics:
  produces:
    - <topic_name>      # если фича publisher
  consumes:
    - <topic_name>      # если фича consumer
```

### 5. Quality gate

Запустить:
```bash
python3 tools/proto-contract-auditor/check_proto_map.py
python3 tools/traceability-checker/check.py
```

Если есть `make`:
```bash
make docs-validate
```

## Rules

- **Не** трогать generated proto.
- **Не** создавать code для producer/consumer — это task для code-implementer.
- **Не** удалять / переименовывать существующий топик (это breaking change → требует ADR).
- Имя топика — строчные буквы, `.`-разделение, без `_`, без CAPS.
- `EventMeta` обязательно во всех новых message types (см. proto-contract-designer rules).
- Topic в `create_topics.sh` должен быть **idempotent** (`--if-not-exists` или эквивалент в `create_topic` helper).
- Документация добавляется **до** реализации producer/consumer.

## Output

После выполнения вернуть пользователю:

1. **Изменённые файлы**:
   - `infra/kafka/create_topics.sh` (новая строка `create_topic`)
   - `docs/06-api/messaging/<topic>.md` (новый файл)
   - `docs/02-system/features/F-XX-*/feature.yaml` (обновлены kafkaTopics)

2. **Команда применения** (на dev-хосте):
   ```bash
   ssh nik@ubuntu-dev "cd /home/nik/cont_exchange_v2/infra && \
     docker compose -f docker-compose.dev.yml restart topics-init"
   ```
   После чего:
   ```bash
   docker exec infra-redpanda-1 rpk topic list | grep <topic_name>
   ```

3. **Рекомендация следующего шага**:
   - Если proto MessageName новый → proto-contract-designer
   - Если proto существует → implementation-planner для producer/consumer task
