# 07-data

## Назначение

Модели данных, схемы хранилищ, политики ретенции и происхождение данных (data lineage).

## Состав

- [data-overview.md](data-overview.md) — общая карта хранилищ (PostgreSQL / ClickHouse / Redis / Kafka).
- [schemas/](schemas/) — детальные схемы таблиц (планируется).

## Хранилища

| Store | Назначение | Источник истины |
| --- | --- | --- |
| PostgreSQL | OLTP: пользователи, заявки, балансы, лимиты | `infra/postgres/` |
| ClickHouse | OLAP: fills, batch_results, marketdata, агентские логи | `infra/clickhouse/` |
| Redis | кэш тикеров, sessions | `cpp/market_data/`, `cpp/gateway/` |
| Kafka / Redpanda | событийная шина | `infra/kafka/create_topics.sh` |

## Связанные разделы

- [../04-domain/entities.md](../04-domain/entities.md) — доменные сущности.
- [../06-api/messaging/topics.md](../06-api/messaging/topics.md) — Kafka-топики.
- [../08-infrastructure/](../08-infrastructure/) — деплой хранилищ.
- [../../specs/domain/storage-schema.yaml](../../specs/domain/storage-schema.yaml) — машинная схема.
