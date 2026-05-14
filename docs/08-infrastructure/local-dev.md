---
id: DOC-INFRA-LOCAL-DEV
phase: 08-infrastructure
status: draft
owner: core-team
related:
  - infra/docker-compose.dev.yml
  - Makefile
  - Development.md
---

# Local Dev Environment

## TL;DR

```bash
make dev-up      # подъём Kafka + Postgres + ClickHouse + Redis
make build       # сборка C++ сервисов
make run-mvp     # старт минимального стенда
make dev-down    # остановка
```

Подробности — в [../../Development.md](../../Development.md).

## Состав стенда

| Сервис | Порт | Источник |
| --- | --- | --- |
| Redpanda (Kafka) | 9092 | `infra/docker-compose.dev.yml` |
| PostgreSQL | 5432 | `infra/docker-compose.dev.yml` |
| ClickHouse | 8123 / 9000 | `infra/docker-compose.dev.yml` |
| Redis | 6379 | `infra/docker-compose.dev.yml` |
| gateway | 8080 | `cpp/gateway/` |

## Создание Kafka-топиков

```bash
bash infra/kafka/create_topics.sh
```

См. список топиков в [../06-api/messaging/topics.md](../06-api/messaging/topics.md).

## Проверки

- `make test` — unit + integration тесты.
- `make lint` — линтеры.
- `python3 tools/traceability-checker/check.py` — проверка трассировки.

## Связанные документы

- [infra-overview.md](infra-overview.md)
- [ci-cd.md](ci-cd.md)
- [../11-operations/runbook.md](../11-operations/runbook.md)
