---
id: DOC-OPS-RUNBOOK
phase: 11-operations
status: draft
owner: core-team
related:
  - infra/docker-compose.dev.yml
  - cpp/observability/
---

# Runbook (MVP)

> Статус: **draft**. Прод-эксплуатация не запущена. Описаны процедуры для local
> и предполагаемого staging.

## Старт стенда

```bash
make dev-up           # Kafka / Postgres / ClickHouse / Redis
bash infra/kafka/create_topics.sh
make build && make run-mvp
```

## Остановка

```bash
make dev-down
```

## Проверка здоровья

| Что | Команда / эндпоинт | Что ожидать |
| --- | --- | --- |
| gateway | `curl localhost:8080/healthz` | `ok` |
| Kafka топики | `rpk topic list` | все из [../06-api/messaging/topics.md](../06-api/messaging/topics.md) |
| Postgres | `psql -h localhost -U fob -c 'select 1'` | `1` |
| ClickHouse | `curl localhost:8123/ping` | `Ok.` |

## Типовые инциденты

### Kafka consumer lag

1. Проверить отстающий consumer-group: `rpk group describe <group>`.
2. Если процесс мёртв — рестарт через `systemctl` / docker.
3. При репротестинге — fix idempotency (см. [../06-api/messaging/topics.md](../06-api/messaging/topics.md)).

### Матчинг не сходится (residual_norm высок)

1. Снять `BatchDiagnostics` из `batch.outputs`.
2. Сверить с baseline из [../../legacy_mvp/](../../legacy_mvp/).
3. Эскалация — owner matching-fob-core.

### Расхождение балансов (ledger vs CEX)

1. Заморозить вывод средств (флаг в risk-manager).
2. Сравнить `execution.reports` с venue API.
3. См. [../05-components/external-venues/](../05-components/external-venues/).

## Деплой

> Прод-деплой не реализован. Цель: `infra/k8s/` манифесты + GitHub Actions.

## Связанные документы

- [../08-infrastructure/](../08-infrastructure/)
- [../05-components/observability-reporting/](../05-components/observability-reporting/)
- [../10-testing/test-strategy.md](../10-testing/test-strategy.md)
