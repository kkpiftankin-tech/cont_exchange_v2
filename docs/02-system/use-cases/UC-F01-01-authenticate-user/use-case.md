# UC-F01-01. Аутентификация пользователя

## Feature

- [F-01. Authentication & Identity](../../features/F-01-auth-and-identity/)

## Primary Actor

Trader / Market Maker / Operator

## Supporting Actors

- KYC Provider (планируется)

## Preconditions

- Пользователь зарегистрирован.
- Сервис Auth & Identity доступен.

## Trigger

Пользователь вводит логин/пароль или открывает API c API-ключом.

## Main Flow

1. Пользователь отправляет credentials.
2. Auth & Identity проверяет учётные данные.
3. Auth & Identity выдаёт session token (JWT).
4. Дальнейшие запросы проходят с этим токеном.

## Alternative Flows

### A1. Неверные credentials

1. Auth возвращает 401.
2. UI показывает ошибку.

## Postconditions

- Сессия активна.
- Запись о входе в audit log.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F01-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F01-UC-F01-01-services.md)

## Related Contracts

- (планируется) `POST /v1/auth/login` в [../../../06-api/rest/](../../../06-api/rest/)

## Related Components

- [auth-identity](../../../05-components/auth-identity/overview.md) (планируется)
- [gateway](../../../05-components/gateway/overview.md)

## Related Data

- (планируется) таблица `users`, `sessions` в [../../../07-data/](../../../07-data/)
