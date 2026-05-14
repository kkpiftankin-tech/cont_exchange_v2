# 11-operations

## Назначение

Эксплуатация системы в проде: runbook'и, SLO, on-call процедуры, incident-response.

## Состав

- [deployment-guide.md](deployment-guide.md) — пошаговый план запуска (локально, удалённо, CI/CD).
- [server-sizing.md](server-sizing.md) — расчёт ресурсов сервера и выбор провайдера (FirstVDS / Hetzner / DO).
- [runbook.md](runbook.md) — общий runbook (старт/стоп/инциденты).
- `slo.md` — целевые SLO/SLI (planned).
- `on-call.md` — дежурство, эскалация (planned).
- `incidents/` — постмортемы (planned).

## Связанные разделы

- [../08-infrastructure/](../08-infrastructure/) — инфраструктура.
- [../05-components/observability-reporting/](../05-components/observability-reporting/) — наблюдаемость.
- [../06-api/messaging/topics.md](../06-api/messaging/topics.md) — Kafka-топики (важно для on-call).
