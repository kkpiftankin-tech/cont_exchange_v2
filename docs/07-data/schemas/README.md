---
id: DOC-DATA-SCHEMAS
phase: 07-data
status: planned
owner: core-team
---

# Data Schemas (per-entity)

> **Статус:** planned. Раздел зарезервирован под per-table DDL и spec-files.

Текущие schemas объединены в:

- [../oltp-schema.md](../oltp-schema.md) — PostgreSQL (operational store)
- [../olap-schema.md](../olap-schema.md) — ClickHouse (analytics store)

Machine-readable spec — в [../../../specs/domain/storage-schema.yaml](../../../specs/domain/storage-schema.yaml).

По мере роста проекта каждая важная таблица получит отдельный файл по шаблону `<table>.md` с полным DDL, индексами, retention, миграциями.
