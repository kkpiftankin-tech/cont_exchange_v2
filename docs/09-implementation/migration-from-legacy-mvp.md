# Компонент: legacy-mvp

Старый MVP биржи на Go (+ React frontend, C++ matching_engine). Сохранён в [legacy_mvp/](../../legacy_mvp/) как **справочный материал и источник миграционной карты**, не основной target для новых фич.

## Состав

| Каталог | Стек | Назначение |
|---|---|---|
| [legacy_mvp/auth/](../../legacy_mvp/auth/) | Go | Аутентификация |
| [legacy_mvp/main/](../../legacy_mvp/main/) | Go + Swagger | Главное API (управление ордерами) |
| [legacy_mvp/matching_engine/](../../legacy_mvp/matching_engine/) | C++ | Старый matching engine |
| [legacy_mvp/executor/](../../legacy_mvp/executor/) | Go | Исполнение ордеров через Binance API |
| [legacy_mvp/fetcher/](../../legacy_mvp/fetcher/) | Go | Источник рыночных данных |
| [legacy_mvp/generator/](../../legacy_mvp/generator/) | Go | Генератор синтетических ордеров |
| [legacy_mvp/receiver/](../../legacy_mvp/receiver/) | Go | Приёмник внешних сигналов |
| [legacy_mvp/reporter/](../../legacy_mvp/reporter/) | Go | Отчёты |
| [legacy_mvp/frontend/](../../legacy_mvp/frontend/) | React | UI |
| [legacy_mvp/pkg/](../../legacy_mvp/pkg/) | Go | Общие пакеты (config, jaeger, middlewares, client) |

## Миграционная карта

См. [migration-map.md](migration-map.md).

## Принципы работы с legacy_mvp

1. **Никаких новых фич в legacy_mvp.** Все фичи реализуются в C++ контуре в `cpp/`.
2. **legacy_mvp может оставаться запущенным** для сравнительного тестирования.
3. **При сомнении в архитектурном решении** — смотрим, как делали в legacy, как историческую отсылку.
4. **Никакого общего proto/Kafka между legacy и новым контуром.** Свои каналы.
