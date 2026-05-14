---
id: DOC-IMPL-SERVICES
phase: 09-implementation
status: planned
owner: core-team
---

# Implementation: services (per-service implementation notes)

> **Статус:** planned. Раздел зарезервирован под детали реализации каждого C++ сервиса.

Текущая структура:

- High-level overview — [../implementation-overview.md](../implementation-overview.md)
- Per-component docs — [../../05-components/](../../05-components/) (одна папка на сервис)
- Build/CI — [../../08-infrastructure/](../../08-infrastructure/) и [../../11-operations/deployment-guide.md](../../11-operations/deployment-guide.md)
- Code map (auto-generated) — [../../generated/code-map.md](../../generated/code-map.md)

По мере появления нетривиальных архитектурных решений в коде, каждый сервис получит свой `<service>.md` здесь с deep-dive деталями реализации (threading model, lifecycle, internal data structures).
