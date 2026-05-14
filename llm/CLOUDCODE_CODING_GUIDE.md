# Cloud Code / Coding Agent Guide

Этот документ — обязательное чтение для coding agent (включая Claude Code / GitHub Copilot Workspace / других LLM-агентов), которые будут править C++ код в этом репозитории.

## Перед началом реализации

Coding agent ДОЛЖЕН прочитать:

1. `docs/02-system/features/F-XX-*/feature.yaml` — целевая фича
2. `docs/02-system/features/F-XX-*/trd.md` (или `README.md`) — технические требования
3. `docs/02-system/features/F-XX-*/acceptance-criteria.md` — критерии готовности
4. `specs/domain/entities.yaml` — сущности и поля
5. `specs/domain/relationships.yaml` — связи между сервисами
6. `specs/domain/code-map.yaml` — где лежит код
7. `specs/contracts/proto-map.yaml` — proto-контракты
8. Соответствующие `contracts/proto/fob/**/*.proto`
9. `docs/05-components/<id>/component.yaml` — целевой компонент
10. `docs/05-components/<id>/README.md` — известные баги и ограничения

## Категорически запрещено

- ❌ Менять `cpp/**` без указания feature ID в commit message
- ❌ Менять `contracts/proto/**` без отдельного contract PR + обновления `proto-map.yaml`
- ❌ Менять risk / ledger / matching логику без сопровождающих тестов
- ❌ Удалять требования или известные баги из документации (только переводить статус)
- ❌ Добавлять секреты, токены, пароли в любые файлы
- ❌ Править `legacy_mvp/**` как основной target
- ❌ Использовать `double`/`float` для денежных величин (только `Decimal`)
- ❌ Использовать `--no-verify` при коммитах
- ❌ Пушить в `main` напрямую

## Обязательно при PR

- ✅ В commit message указать feature ID: `[F-04] short description`
- ✅ В PR description перечислить:
  - Какие файлы изменены и в каких категориях (code / docs / specs / contracts / infra)
  - Какие acceptance criteria покрыты
  - Какие новые тесты добавлены
  - Изменены ли proto-контракты (если да: breaking / non-breaking)
  - Какие открытые вопросы остались
- ✅ Запустить локально: `make -f Makefile.docs validate-docs`
- ✅ Обновить traceability при добавлении / удалении файлов из feature scope

## Шаблон commit message

```
[F-XX] Краткое описание (не более 70 символов)

Подробности (зачем, что меняется, известные следствия).

Closes: <acceptance-criteria-id>
Refs: docs/02-system/features/F-XX-*/feature.yaml
```

## Если требуется решение, не описанное в документации

1. Остановиться
2. Зафиксировать вопрос в `docs/02-system/features/F-XX/open-questions.md`
3. Не делать предположений
4. Спросить пользователя

## Стиль кода

- C++20, `-Wall -Wextra -Wpedantic`
- Heading-style комментарии на русском (см. существующие файлы как референс)
- Никаких `using namespace std;` в headers
- RAII везде; никаких голых `new`/`delete`
- Логирование через `cex::common::log_json`, не через `std::cout` напрямую
- Время через `cex::common::now_ts`, не `std::chrono::system_clock::now()` напрямую
