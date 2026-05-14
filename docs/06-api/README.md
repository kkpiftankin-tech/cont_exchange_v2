# 06-api

## Назначение

Контракты межсервисного и внешнего общения: REST, gRPC, Kafka.

## Что здесь хранится

- [api-overview.md](api-overview.md) — обзор всех контрактов и принципов
- [rest/](rest/) — REST API (OpenAPI)
- [grpc/](grpc/) — gRPC сервисы (proto)
- [messaging/](messaging/) — Kafka topics

Планируется:

- `errors.md` — каталог ошибок и кодов
- `idempotency.md` — правила идемпотентности
- `versioning.md` — правила версионирования

## Source of truth

Сами proto-схемы — в [../../contracts/proto/](../../contracts/proto/). Документация только описывает и ссылается, не дублирует.

См. [../../specs/contracts/proto-map.yaml](../../specs/contracts/proto-map.yaml).

## Связанные разделы

- [../05-components/](../05-components/) — какой компонент чем владеет
- [../07-data/](../07-data/) — реляционная модель
- [../03-architecture/communication.md](../03-architecture/communication.md) — стратегия общения

## Статус

draft

## Ответственные

core-team
