# 09-implementation

## Назначение

Внутренние детали реализации компонентов и сервисов: конкретные классы, модули,
shared-библиотеки, миграция с legacy MVP.

В отличие от [../05-components/](../05-components/), где описаны **роли и контракты**,
здесь — **код и реализационные решения**.

## Состав

- [implementation-overview.md](implementation-overview.md) — карта реализации.
- [cpp-common.md](cpp-common.md) — shared C++ библиотека.
- [migration-from-legacy-mvp.md](migration-from-legacy-mvp.md) — миграция со старого MVP.
- [migration-map.md](migration-map.md) — карта соответствий legacy ↔ новые модули.
- [services/](services/) — детальные документы по сервисам (планируется).

## Связанные разделы

- [../05-components/](../05-components/) — контракты компонентов.
- [../06-api/](../06-api/) — публичные API.
- [../../cpp/](../../cpp/) — собственно исходники.
- [../../legacy_mvp/](../../legacy_mvp/) — legacy реализация.
