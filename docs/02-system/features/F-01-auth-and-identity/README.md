# F-01 — Registration & Authentication

> **Статус:** Not implemented.

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
