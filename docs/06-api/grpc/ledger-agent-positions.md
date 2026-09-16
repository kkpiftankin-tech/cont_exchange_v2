# gRPC Method: LedgerService/GetAgentPositions

## Status

**Implemented** (T-F18-202/203/204, ADR-061 §7, ADR-063; Э2-шаг-2, 2026-09-16). Метод добавлен в `contracts/proto/fob/ledger/v1/ledger.proto`; `ledger_uc.cpp` накапливает `c ← c + f` per `(agent_id, asset, venue)` в write-through кэше и опционально в PG (`ce_agent_position`, `PostgresAgentPositionRepository`). Фактическая форма контракта расходится с исходным черновиком ниже (см. правки inline) — `agent_kind` осталась строкой (не общий `enum AgentKind`, чтобы не тянуть `treasury.proto` в `ledger.proto`), добавлено поле `in_flight` (зарезервировано под Э3), `updated_at` заменён на `int64 updated_at_ms` (консистентно с остальным `ledger.proto`, напр. `BatchNopSnapshot.event_time_ms`).

## Purpose

Вернуть **позиции виртуальных контрагентов (агентов)** CE v2 — знаковый накопленный неисполненный поток `c_j` по каждому агенту (см. [ce-agent-position.md](../../07-data/ce-agent-position.md)). Нужен:

- **matching** — A2 читает `c_j` для сдвига якоря `σ*_j = m_j + ρ_j · c_j` и A7 для проверки полосы `|c_j| > q_j` (эмиссия наружу);
- **risk** — мониторинг выхода за полосу;
- **frontend-api / BFF** — панель позиций агентов;
- **observability**.

Read-only, additive к `LedgerService` — **не breaking** (новый RPC не трогает существующие сообщения/номера полей).

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

Фактическая форма (`contracts/proto/fob/ledger/v1/ledger.proto`, после `ApplyNodeTransferRequest`):

```proto
message AgentPosition {
  string agent_id = 1;                        // "T_BTC" | "A_BTC_BO" (= имя ребра CeEdge.name)
  string agent_kind = 2;                       // "translator" | "arbitrageur" | "" (неизвестно)
  string asset = 3;                            // "BTC" | "USDT" ...
  string venue = 4;                            // площадка узла; "" = route-level (арбитражёр, V=2)
  fob.common.v1.Decimal position = 5;          // c_j, ЗНАКОВАЯ, накопленная клирингом
  fob.common.v1.Decimal in_flight = 6;         // committed (Э3+), ЗНАКОВАЯ, пока всегда 0
  int64 updated_at_ms = 7;                     // unix ms последнего изменения
  string last_batch_id = 8;                    // последний такт, менявший позицию
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

Примечания к контракту (актуализировано по факту реализации):

- `position` — `fob.common.v1.Decimal` (деньги/объём, CLAUDE.md §9), **без** ограничения знака (позиция агента знаковая — A6, проверено тестом на отрицательную позицию).
- `agent_kind` — **строка** ("translator" | "arbitrageur" | ""), НЕ общий `enum AgentKind` из черновика: совпадает с PG-хранением (`ce_agent_position.agent_kind TEXT CHECK (... IN ('translator','arbitrageur'))`, [ce-agent-position.md](../../07-data/ce-agent-position.md)) и не создаёт зависимость `ledger.proto → treasury.proto` (где живёт `AgentKind`) ради одного поля.
- `in_flight` — добавлено к черновику: зарезервировано для Э3 (полоса `±q`, T-F18-304); до включения `CE_AGENT_BAND` всегда `0`.
- `updated_at_ms` (`int64`, не `google.protobuf.Timestamp`) — консистентно с остальными `int64`-таймстемпами `ledger.proto` (напр. `BatchNopSnapshot.event_time_ms`).
- **Идемпотентность:** RPC read-only, идемпотентен по конструкции — без явного `idempotency_key` (тот же паттерн, что `GetNodeBalances` / `GetBalances`). Чтение — из write-through кэша `LedgerUseCases::agent_positions_` (под `mu_`), не из PG на каждый вызов; кэш синхронно перечитывается из PG при старте сервиса (`SetAgentPositionRepo` → `LoadAgentPositionsFromRepo`) — позиция агента переживает рестарт.
- `venue = ""` для арбитражёра — явный сентинел «route-level» (позиция арбитражёра скалярна, не per-venue); конвенция ключа при `V>2` — открытый вопрос ADR-061 (см. data-doc).

## Used In Features

- F-18 v2 (CE-агенты v2, виртуальные контрагенты)

## Used In Use Cases

- (TODO) UC F-18 v2 — CE tact clearing with agent positions

## Related Components

- [ledger](../../05-components/ledger/overview.md), matching, [risk-manager](../../05-components/risk-manager/overview.md)

## Related Data Objects

- PG [`ce_agent_position(agent_id, asset, venue → position, updated_at, last_batch_id)`](../../07-data/ce-agent-position.md) (planned, T-F18-007..011)

## Related Contracts

- Kafka [`ce.position.delta`](../messaging/ce-position-delta.md) (`AgentDelta`) — источник апдейтов позиции, которую этот RPC читает.

## Related ADR

- ADR-061 (TODO) — модель позиции агента v2; supersedes ADR-057 (`GetNodeBalances` / `target`) и ADR-058 (committed state).
