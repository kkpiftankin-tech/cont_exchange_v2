---
id: DOC-API-REST
phase: 06-api
status: planned
owner: core-team
---

# REST API

> **Статус:** planned. Раздел зарезервирован под per-endpoint спецификации REST-API gateway.

Текущие REST-эндпоинты gateway документируются inline в [../../05-components/gateway/overview.md](../../05-components/gateway/overview.md). По мере роста API каждый эндпоинт получит отдельный файл в этом каталоге по шаблону `<endpoint-slug>.md`.

## Принцип

См. [../api-overview.md](../api-overview.md) — общая методология контрактов.

Per-method gRPC — в [../grpc/](../grpc/), per-topic Kafka — в [../messaging/](../messaging/).

## Запланированные эндпоинты

См. сервис-секвенции, где упоминаются REST-вызовы:

- `POST /v1/auth/login`, `POST /v1/auth/refresh` ([SEQ-F01](../../05-components/sequences/SEQ-F01-UC-F01-01-services.md))
- `POST /v1/flow-orders` ([SEQ-F02](../../05-components/sequences/SEQ-F02-UC-F02-01-services.md))
- `PATCH /v1/flow-orders/{id}`, `DELETE /v1/flow-orders/{id}` ([SEQ-F03](../../05-components/sequences/SEQ-F03-UC-F03-01-services.md))
- `GET /v1/positions` ([SEQ-F06](../../05-components/sequences/SEQ-F06-UC-F06-01-services.md))
- `POST /v1/combo-orders` ([SEQ-F09](../../05-components/sequences/SEQ-F09-UC-F09-01-services.md))
- `POST /v1/mm/curve` ([SEQ-F10](../../05-components/sequences/SEQ-F10-UC-F10-01-services.md))
- `GET /v1/reports/post-trade` ([SEQ-F13](../../05-components/sequences/SEQ-F13-UC-F13-01-services.md))
- `POST /v1/deposits/initiate` ([SEQ-F14](../../05-components/sequences/SEQ-F14-UC-F14-01-services.md))
- `POST /v1/admin/kill-switch` ([SEQ-F16](../../05-components/sequences/SEQ-F16-UC-F16-01-services.md))

WebSocket streams (tickers, batch.outputs, fills) — см. [../../11-operations/deployment-guide.md](../../11-operations/deployment-guide.md) §7.
