# gRPC Method: AuthService/ValidateToken

## Status

TODO / planned

## Purpose

Проверить валидность access token и вернуть claims (`user_id`, `roles`, `expires_at`, optional `account_id`). Gateway вызывает этот метод на каждом аутентифицируемом запросе — либо использует кэш с коротким TTL.

## Transport

gRPC

## Service

`fob.auth.v1.AuthService` (planned)

## Method (planned)

```proto
rpc ValidateToken(ValidateTokenRequest) returns (ValidateTokenResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (middleware на каждом запросе)

## Callee

- [auth-identity](../../05-components/auth-identity/overview.md) (planned)

## Schema

TODO. Предлагаемая форма:

```proto
message ValidateTokenRequest {
  fob.common.v1.EventMeta meta = 1;
  string access_token = 2;
}

message TokenClaims {
  string user_id = 1;
  repeated string roles = 2;
  string account_id = 3;
  google.protobuf.Timestamp expires_at = 4;
}

message ValidateTokenResponse {
  fob.common.v1.EventMeta meta = 1;
  bool valid = 2;
  TokenClaims claims = 3;
  fob.common.v1.Error error = 4;
}
```

## Performance

Hot path — целесообразно поддержать JWT с локальной валидацией без сетевого hop, либо короткий локальный кэш в gateway.

## Used In Features

- [F-01. Auth and Identity](../../02-system/features/F-01-auth-and-identity/)
- Каждая аутентифицируемая фича (F-02..F-16) косвенно зависит от валидации токена

## Used In Use Cases

- [UC-F01-01](../../02-system/use-cases/UC-F01-01-authenticate-user/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F01-UC-F01-01-services](../../05-components/sequences/SEQ-F01-UC-F01-01-services.md)

## Related Components

- [auth-identity](../../05-components/auth-identity/overview.md)
- [gateway](../../05-components/gateway/overview.md)

## Related Data Objects

- `sessions` (OLTP — planned)

## Source Fragments

- IN-001-FR-016 (FR-AUTH-002)
- IN-001-FR-022
