---
id: CMP-AUTH-IDENTITY
phase: 05-components
status: not-implemented
component: auth-identity
related:
  - docs/02-system/features/F-01-auth-and-identity/
---

# Component: Auth & Identity

## Назначение

Регистрация, логин, управление сессиями и ролями (demo/client/provider/operator/admin).
Защищает все остальные сервисы через токен.

## Ответственность

- Регистрация пользователей.
- Логин / logout / refresh.
- Управление сессиями и токенами.
- Управление ролями.
- Интеграция с KYC/AML провайдером.

## Не отвечает за

- Бизнес-логику ордеров — это `order-flow`.
- Risk — это `risk-manager`.

## Входы

- HTTP запросы регистрации/логина через gateway.
- KYC-сигналы от внешних провайдеров.

## Выходы

- Токены сессий.
- Контекст user_id/role для downstream gRPC.

## Данные

- PostgreSQL `users`, `sessions` (см. [../../07-data/schemas/](../../07-data/schemas/)).

## Связанная фича

- F-01 — [../../02-system/features/F-01-auth-and-identity/](../../02-system/features/F-01-auth-and-identity/).

## Participates In Features

- [F-01](../../02-system/features/F-01-auth-and-identity/)

## Participates In Use Cases

- [UC-F01-01](../../02-system/use-cases/UC-F01-01-authenticate-user/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F01-UC-F01-01-services](../sequences/SEQ-F01-UC-F01-01-services.md)

## Owned Contracts

- (planned) `fob.auth.v1.AuthService` — [../../06-api/grpc/](../../06-api/grpc/)
- (planned) REST `POST /v1/auth/login`, `/v1/auth/refresh` — [../../06-api/rest/](../../06-api/rest/)

## Produced Events

- (planned) `auth.events` (login / logout / failed_login)

## Consumed Events

- (none)

## Data Access

- (planned) `users`, `sessions` в PostgreSQL — [../../07-data/data-overview.md](../../07-data/data-overview.md)

## Статус

Not implemented. gateway сейчас работает без авторизации.
