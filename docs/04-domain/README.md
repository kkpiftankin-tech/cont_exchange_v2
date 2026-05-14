# 04-domain

## Назначение

Доменный уровень: формализация предметной области независимо от реализации.

## Что здесь хранится

- [domain-overview.md](domain-overview.md) — общая картина домена
- [ubiquitous-language.md](ubiquitous-language.md) — технические термины (CSLO, FOB, IS …)
- [entities.md](entities.md) — сущности и их связи
- [events/](events/) — каталог доменных событий

Планируется:

- `value-objects.md`, `aggregates.md`, `business-rules.md`, `invariants.md`, `formulas.md`, `domain-diagram.md`

## Как читать

Разработчик: ubiquitous-language → entities → events. Архитектор: domain-overview → invariants.

## Связанные разделы

- [../01-business/glossary.md](../01-business/glossary.md) — бизнес-словарь
- [../06-api/](../06-api/) — реализация контрактов
- [../../specs/domain/entities.yaml](../../specs/domain/entities.yaml) — машинно-читаемая модель

## Статус

draft

## Ответственные

core-team
