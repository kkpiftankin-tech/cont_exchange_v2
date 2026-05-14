# Explanatory Note — source document (HISTORICAL PLACEHOLDER)

> **⚠️ Этот файл — historical placeholder, не источник.**
>
> Полный текст пояснительной записки сохранён в [2026-05-13-EXPLANATORY-NOTE-full.md](2026-05-13-EXPLANATORY-NOTE-full.md) после восстановления из mojibake (2026-05-13). Используйте полный архив для любых ссылок и трассировки.
>
> Ссылки на пути ниже (`docs/glossary.md`, `docs/architecture/*`, `docs/components/`, `docs/features/`) относятся к старой структуре репозитория и **больше не валидны** после миграции на 11-папочную структуру. Актуальная раскладка — в [IN-001.fragment-map.md](IN-001.fragment-map.md) и [docs/traceability/source-to-artifact-map.md](../docs/traceability/source-to-artifact-map.md).

Этот файл сохранён в inbox как **исходник полной пояснительной записки** проекта:
концепции, BRD, роли, состав компонентов, перечень фич F-01…F-18, схема PostgreSQL OLTP
и ClickHouse OLAP. Кодировка исходника была повреждена (mojibake), поэтому здесь —
не сам текст, а ссылка на разделы и куда он "разложен" по overlay-документации.

## Куда инкорпорирована информация

| Раздел исходника | Куда разложено |
|---|---|
| §1 «Базовые рыночные понятия», §3 «CSLO», §5 «Стоимость и риск» | [docs/glossary.md](../glossary.md) |
| §4 «Участники и роли», BRD §4.1 | [docs/architecture/roles-and-personas.md](../architecture/roles-and-personas.md) |
| §8 «Структуры данных и сервисы», БД 1 PostgreSQL, БД 2 ClickHouse | [docs/architecture/data-model.md](../architecture/data-model.md), [specs/domain/storage-schema.yaml](../../specs/domain/storage-schema.yaml) |
| §«Состав компонент системы» (10 контейнеров) | [docs/components/](../components/) (расширено в каждом README) |
| §«Фичи проекта» F-01…F-18 | [docs/features/](../features/) (новые F-01, F-03, F-08, F-10, F-13, F-14, F-16, F-17) |
| §«Трассировка требований» | [specs/domain/feature-component-map.yaml](../../specs/domain/feature-component-map.yaml) и [specs/domain/traceability.yaml](../../specs/domain/traceability.yaml) |

## Статус

`processed-incorporated` — все ключевые сведения уже разложены по постоянным файлам.
Исходник остаётся в inbox как референс и для будущих сверок.
