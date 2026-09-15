# gRPC Method: LedgerService/GetAgentPositions

## Status

**Planned-contract** (proto-правка — стадия кода F-18 v2, T-F18-007..011; ADR-061). Метод ещё
НЕ добавлен в `contracts/proto/fob/ledger/v1/ledger.proto` — этот документ описывает целевой
контракт до реализации (docs-first, CLAUDE.md §11.3).

## Purpose

Вернуть **позиции виртуальных контрагентов (агентов)** CE v2 — знаковый накопленный
неисполненный поток `c_j` по каждому агенту (см.
[ce-agent-position.md](../../07-data/ce-agent-position.md)). Нужен:

- **matching** — A2 читает `c_j` для сдвига якоря `σ*_j = m_j + ρ_j · c_j` и A7 для проверки
  полосы `|c_j| > q_j` (эмиссия наружу);
- **risk** — мониторинг выхода за полосу;
- **frontend-api / BFF** — панель позиций агентов;
- **observability**.

Read-only, additive к `LedgerService` — **не breaking** (новый RPC не трогает существующие
сообщения/номера полей).

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc GetAgentPositions(GetAgentPositionsRequest) returns (GetAgentPositionsResponse);
```

## Caller

- [matching](../../05-components/matching/overview.md) (A2 сдвиг якоря, A7 полоса)
- [risk-manager](../../05-components/risk-manager/overview.md) (мониторинг полосы)
- frontend-api / BFF, observability

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

```proto
message AgentPosition {
  string agent_id = 1;                        // "Q_BIN" | "Q_KRK" | "T_BTC" | "T_USD" ...
  fob.common.v1.AgentKind agent_kind = 2;     // TRANSLATOR | ARBITRAGEUR
  string asset = 3;                           // "BTC" | "USD" ...
  string venue = 4;                           // площадка узла; "" — route-level (арбитражёр, V=2)
  fob.common.v1.Decimal position = 5;         // c_j, ЗНАКОВАЯ, накопленный неисполненный поток
  google.protobuf.Timestamp updated_at = 6;
  string last_batch_id = 7;                   // последний такт, менявший позицию
}
message GetAgentPositionsRequest {
  fob.common.v1.EventMeta meta = 1;
  repeated string agent_ids = 2;              // пусто = все агенты
  repeated string assets = 3;                 // опц. фильтр; пусто = все активы
  repeated string venues = 4;                 // опц. фильтр; пусто = все площадки
}
message GetAgentPositionsResponse {
  fob.common.v1.EventMeta meta = 1;
  repeated AgentPosition positions = 2;
}
```

Примечания к контракту:

- `position` — `fob.common.v1.Decimal` (деньги/объём, CLAUDE.md §9), **без** ограничения знака
  (позиция агента знаковая — A6).
- `agent_kind` — общий `enum AgentKind { AGENT_KIND_UNSPECIFIED=0, AGENT_KIND_TRANSLATOR=1,
  AGENT_KIND_ARBITRAGEUR=2 }` в `fob/common/v1/common.proto` (используется несколькими пакетами,
  дублировать по пакетам не следует).
- **Идемпотентность:** RPC read-only, идемпотентен по конструкции — без явного
  `idempotency_key` (тот же паттерн, что `GetNodeBalances` / `GetBalances`).
- `venue = ""` для арбитражёра — явный сентинел «route-level» (позиция арбитражёра скалярна, не
  per-venue); конвенция ключа при `V>2` — открытый вопрос ADR-061 (см. data-doc).

## Used In Features

- F-18 v2 (CE-агенты v2, виртуальные контрагенты)

## Used In Use Cases

- (TODO) UC F-18 v2 — CE tact clearing with agent positions

## Related Components

- [ledger](../../05-components/ledger/overview.md), matching,
  [risk-manager](../../05-components/risk-manager/overview.md)

## Related Data Objects

- PG [`ce_agent_position(agent_id, asset, venue → position, updated_at, last_batch_id)`](../../07-data/ce-agent-position.md)
  (planned, T-F18-007..011)

## Related Contracts

- Kafka [`ce.position.delta`](../messaging/ce-position-delta.md) (`AgentDelta`) — источник
  апдейтов позиции, которую этот RPC читает.

## Related ADR

- ADR-061 (TODO) — модель позиции агента v2; supersedes ADR-057 (`GetNodeBalances` / `target`)
  и ADR-058 (committed state).
