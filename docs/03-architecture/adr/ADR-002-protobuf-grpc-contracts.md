# ADR-0002 — Proto-контракты как source of truth

- **Status:** Accepted
- **Date:** 2026-05-13
- **Deciders:** core-team

## Контекст

`contracts/proto/fob/` — каталог с protobuf-схемами для gRPC сервисов и Kafka payloadов. Любая попытка описать сообщения в Markdown / YAML / коде создаёт дрифт.

## Решение

1. **Никаких message-схем в документации** — только ссылки на `.proto` файлы.
2. **`specs/contracts/proto-map.yaml`** — индекс: какой proto-файл → каким фичам → каким компонентам → каким сущностям. CI проверяет, что **каждый** `.proto` файл из `contracts/proto/**` упомянут в `proto-map.yaml`.
3. **Breaking changes только через новую major-версию** (`v2/`). В `v1/`:
   - новые поля только опциональные;
   - удаления через `reserved` (номер тега) + `reserved` (имя поля);
   - переименования = удаление + добавление.
4. **Proto-изменения проходят через отдельный PR**, отдельно от изменений в коде. Сначала approved proto-contract change → потом implementation PR.

## Альтернативы

- **JSON Schema / OpenAPI как primary.** Отклонено: gRPC уже работает на proto, дублирование породит расхождения.
- **AsyncAPI для Kafka.** Принято в будущем дополнить (`specs/contracts/asyncapi/`), но в дополнение, не вместо.

## Последствия

- + Один источник истины для wire-формата
- + Контрактные тесты возможны автоматически (proto-схема даёт runtime валидацию)
- − Нужна дисциплина: правка `.proto` без `proto-map.yaml` блокируется CI
