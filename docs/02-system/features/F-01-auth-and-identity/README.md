# F-01 — Registration & Authentication

> **Статус:** Not implemented.

## 🧭 Navigation Map (IN-013 drill-down)

Эта секция — **карта документации сверху вниз** для фичи.
Каждый уровень имеет свой ответ на «что/как», и каждая ссылка
ведёт на следующий уровень детализации.

```text
   ┌─ Уровень ──────────────┬─ Артефакт ─────────────────────────────────┐
☁️ L0 │ Что система делает    │ Эта страница + L0 system sequence(s) ниже  │
🌊 L1 │ Какие функции у фичи?  │ Use Cases (таблица ниже)                   │
   │ Какие сервисы участвуют?│ L1 service sequences (per-UC)              │
🐟 L2 │ Из каких классов       │ Component overviews + L2 sequences         │
   │ состоит сервис?        │                                            │
💻 src │ Код                    │ cpp/<component>/src/...                    │
   └────────────────────────┴────────────────────────────────────────────┘
```

## 📋 Use Cases (L1 🌊)

| UC | Имя | L0 sequence ☁️ | L1 sequence 🌊 |
| --- | --- | --- | --- |
| [UC-F01-01](../../use-cases/UC-F01-01-authenticate-user/use-case.md) | Authenticate User | [SEQ-UC-F01-01-system](../../use-cases/UC-F01-01-authenticate-user/sequences/SEQ-UC-F01-01-system.md) | [SEQ-F01-UC-F01-01-services](../../../05-components/sequences/SEQ-F01-UC-F01-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| [gateway](../../../05-components/gateway/overview.md) | (L2 sequences pending) |
| [auth-identity](../../../05-components/auth-identity/overview.md) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

## Описание

Управление учётными записями, сессиями и ролями. Все остальные фичи опираются на токен из F-01.

## TODO

- Отдельный сервис `auth-identity` (или встроить в gateway).
- Таблицы `users`, `sessions` в PostgreSQL ([storage-schema.yaml](../../../../specs/domain/storage-schema.yaml)).
- Middleware в gateway, проверяющий токен и пробрасывающий `user_id`+`role` в downstream gRPC.
- Интеграция с KYC/AML провайдером (см. F-14).

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна позволять пользователям безопасно регистрироваться, входить в систему и получать роли (`demo`, `client`, `provider`, `operator`, `admin`).

- Регистрация и логин работают через email/password (расширяемо до SSO).
- Сессия активна, токен валиден, IP логируется.
- KYC-статус управляет переходом из `demo` в `client`/`provider`.
- Сессия может быть отозвана (logout / подозрительная активность).

Источник: IN-001 §6 FR-AUTH-001, FR-AUTH-002.

## Source Fragments

- IN-001-FR-027
- IN-001-FR-028
