---
id: DOC-INDEX
status: draft
owner: core-team
updated_at: 2026-05-13
---

# Documentation index

Документация Continuous Exchange / Flow Order Book устроена **по этапам жизненного цикла** проекта.
Каждая папка `0X-*` отвечает на один большой вопрос. Подробная методология — в [../ЭТАПЫ.md](../ЭТАПЫ.md).

## Карта по этапам

| # | Папка | Вопрос | Главное |
| --- | --- | --- | --- |
| 1 | [01-business/](01-business/) | Зачем мы это строим? | [vision.md](01-business/vision.md), [stakeholders.md](01-business/stakeholders.md), [glossary.md](01-business/glossary.md) |
| 2 | [02-system/](02-system/) | Что должна делать система? | [system-overview.md](02-system/system-overview.md), [features.md](02-system/features.md), [use-cases/](02-system/use-cases/) |
| 3 | [03-architecture/](03-architecture/) | Как устроено в целом? | [architecture-overview.md](03-architecture/architecture-overview.md), [adr/](03-architecture/adr/) |
| 4 | [04-domain/](04-domain/) | Какова предметная область? | [domain-overview.md](04-domain/domain-overview.md), [entities.md](04-domain/entities.md), [ubiquitous-language.md](04-domain/ubiquitous-language.md) |
| 5 | [05-components/](05-components/) | Из каких компонент собрано? | [components-overview.md](05-components/components-overview.md) и `<component>/overview.md` |
| 6 | [06-api/](06-api/) | Какие контракты у компонент? | [api-overview.md](06-api/api-overview.md), [grpc/](06-api/grpc/), [messaging/](06-api/messaging/) |
| 7 | [07-data/](07-data/) | Как хранятся данные? | [data-overview.md](07-data/data-overview.md), [schemas/](07-data/schemas/) |
| 8 | [08-infrastructure/](08-infrastructure/) | Как разворачиваем и эксплуатируем платформу? | [infra-overview.md](08-infrastructure/infra-overview.md), [local-dev.md](08-infrastructure/local-dev.md), [ci-cd.md](08-infrastructure/ci-cd.md) |
| 9 | [09-implementation/](09-implementation/) | Как реализован код? | [implementation-overview.md](09-implementation/implementation-overview.md), [services/](09-implementation/services/) |
| 10 | [10-testing/](10-testing/) | Как доказываем соответствие? | [test-strategy.md](10-testing/test-strategy.md) |
| 11 | [11-operations/](11-operations/) | Как управляем системой в проде? | [runbook.md](11-operations/runbook.md) |

## Сервисные точки входа

- **Новый материал** → [../incoming-docs/](../incoming-docs/)
- **Машинно-читаемые спецификации** → [../specs/](../specs/)
- **LLM-инструкции** → [../llm/](../llm/)
- **Утилиты валидации** → [../tools/](../tools/)
- **Корневой README** → [../README.md](../README.md)
- **Методология** → [../ЭТАПЫ.md](../ЭТАПЫ.md)

## Как читать документацию

- **Бизнес-стейкхолдер:** `01-business/` → `02-system/features.md` → нужная фича в `02-system/features/F-XX-*/`.
- **Архитектор:** `03-architecture/architecture-overview.md` → `03-architecture/adr/` → `05-components/`.
- **Разработчик:** `04-domain/` → `05-components/<your-component>/` → `06-api/` → `09-implementation/services/<your-service>.md`.
- **SRE / Operator:** `08-infrastructure/` → `11-operations/runbook.md`.
- **QA:** `02-system/use-cases/` → `10-testing/test-strategy.md`.

## Статусы документов

| Статус | Значение |
| --- | --- |
| `draft` | Черновик, может меняться без ADR |
| `review` | На ревью |
| `approved` | Утверждён, изменения через ADR / PR |
| `deprecated` | Замещён, оставлен для истории |
