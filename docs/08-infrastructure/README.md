# 08-infrastructure

## Назначение

Окружения, развёртывание, CI/CD, локальная разработка, секреты.

## Состав

- [infra-overview.md](infra-overview.md) — общая карта инфраструктуры.
- [ci-cd.md](ci-cd.md) — конвейеры сборки и проверок.
- [local-dev.md](local-dev.md) — поднятие стенда локально.
- [infra.component.yaml](infra.component.yaml) — машинная карточка.

## Окружения

| Env | Назначение | Источник истины |
| --- | --- | --- |
| local | разработка на ноутбуке | `docker-compose.yml`, `infra/` |
| ci | проверки в GitHub Actions | `.github/workflows/` |
| staging | предпрод (планируется) | — |
| prod | продакшен (планируется) | — |

## Связанные разделы

- [../07-data/](../07-data/) — деплой хранилищ.
- [../11-operations/](../11-operations/) — эксплуатация.
- [../../infra/](../../infra/) — реальные манифесты и скрипты.
