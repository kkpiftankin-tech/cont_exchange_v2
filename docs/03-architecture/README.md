# 03-architecture

## Назначение

Архитектурный уровень: как система устроена в целом, какие контейнеры/сервисы есть, как они общаются.

## Что здесь хранится

- [architecture-overview.md](architecture-overview.md) — высокоуровневая картина
- [component-map.md](component-map.md) — карта компонент → код → доки → proto
- [communication.md](communication.md) — gRPC + Kafka, EDA-паттерн
- [adr/](adr/) — Architecture Decision Records

Планируется:

- `c4-context.md`, `container-diagram.md` — C4 диаграммы
- `service-dependencies.md`, `storage-strategy.md`, `tech-stack.md`, `threat-model.md`

## Как читать

Архитектор: architecture-overview → adr → component-map → communication.

## Связанные разделы

- [../02-system/](../02-system/) — что должна делать система
- [../05-components/](../05-components/) — детали по каждому компоненту
- [../06-api/](../06-api/) — контракты

## Статус

draft

## Ответственные

core-team
