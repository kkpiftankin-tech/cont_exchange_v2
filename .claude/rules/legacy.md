---
paths: ["legacy_mvp/**"]
---

# Работа с legacy_mvp/ (перенесено из CLAUDE.md §23)

`legacy_mvp/` — справочный исходный MVP.

Правила:

- не удалять;
- не смешивать с новым C++ skeleton без явного migration plan;
- можно использовать идеи, UI assets, DB diagrams, прежние сервисные
  паттерны;
- при переносе кода оформлять как отдельный refactoring/migration task;
- не считать legacy architecture источником истины, если она конфликтует с
  `docs/` или `contracts/proto/` (приоритет источников — CLAUDE.md §3.1,
  `legacy_mvp/` стоит там последним).
