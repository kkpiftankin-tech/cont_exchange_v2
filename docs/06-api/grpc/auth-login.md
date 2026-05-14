# gRPC Method: AuthService/Login

## Status

TODO / planned

## Purpose

Аутентифицировать пользователя по credentials (email + пароль, либо 2FA-token, либо external IDP) и вернуть короткоживущий access token + refresh token. В MVP-skeleton полноценная реализация отсутствует — заявка относится к F-01.

## Transport

gRPC

## Service

`fob.auth.v1.AuthService` (planned package; в `contracts/proto/` отсутствует)

## Method (planned)

```proto
rpc Login(LoginRequest) returns (LoginResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `POST /v1/auth/login` → gRPC)

## Callee

- [auth-identity](../../05-components/auth-identity/overview.md) (planned)

## Schema

TODO. Предлагаемая форма:

```proto
message LoginRequest {
  fob.common.v1.EventMeta meta = 1;
  string email = 2;
  string password = 3;        // или OTP-code, или external_idp_token
  string client_fingerprint = 4;
}

message LoginResponse {
  fob.common.v1.EventMeta meta = 1;
  string access_token = 2;
  string refresh_token = 3;
  google.protobuf.Timestamp expires_at = 4;
  fob.common.v1.Error error = 5;
}
```

## Security

- Пароли никогда не логируются.
- Поддержка rate-limit и brute-force защиты.
- Audit-log каждой попытки в `risk_events`.

## Used In Features

- [F-01. Auth and Identity](../../02-system/features/F-01-auth-and-identity/)

## Used In Use Cases

- [UC-F01-01. Login](../../02-system/use-cases/UC-F01-01-authenticate-user/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F01-UC-F01-01-services](../../05-components/sequences/SEQ-F01-UC-F01-01-services.md)

## Related Components

- [auth-identity](../../05-components/auth-identity/overview.md)
- [gateway](../../05-components/gateway/overview.md)

## Related Data Objects

- `users`, `sessions` (OLTP — planned)

## Source Fragments

- IN-001-FR-016 (FR-AUTH-001)
- IN-001-FR-022
