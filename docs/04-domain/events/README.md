---
id: DOC-DOMAIN-EVENTS
phase: 04-domain
status: planned
owner: core-team
---

# Domain events

> **Статус:** planned. Раздел зарезервирован под каталог доменных событий.

Текущие события документируются:

- Kafka payloads — в [../../06-api/messaging/](../../06-api/messaging/) (per-topic)
- Доменные entities — в [../entities.md](../entities.md)
- Бизнес-правила — в `business-rules.md` (planned)

По мере роста проекта каждое значимое доменное событие (`FlowOrderCreated`, `BatchCleared`, `RiskAlert`, etc.) получит отдельный файл с триггерами, payload, consumers, invariants.
