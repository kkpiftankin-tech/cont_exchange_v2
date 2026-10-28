# gRPC Method: LedgerService/GetAgentPnl

## Status

**Proposed** (ADR-087, 2026-10-28). Метод добавляется в `contracts/proto/fob/ledger/v1/ledger.proto` аддитивно; до принятия ADR-087 статус не повышается.

## Purpose

Вернуть **заработок агентов CE** (переводчиков и связок) в USDT, уже посчитанный в ledger: по каждому агенту три группы чисел — внутренняя биржа CE, внешние биржи, итого; в каждой R (реализованный результат), U (нереализованный) и R+U. Формулы, единицы и примеры — [CALC-CE-AGENT-PNL](../../04-domain/calculations/CALC-CE-AGENT-PNL.md), решение — [ADR-087](../../03-architecture/adr/ADR-087-ce-agent-pnl-internal-external-total.md). Нужен:

- **frontend-api / BFF** — вкладка «PnL» (`/hedge-pnl-live`): таблица «Заработок агентов». BFF пересылает ответ в JSON и арифметики над R и U не делает ([REST](../rest/agent-pnl.md)).

Read-only, additive к `LedgerService` — **не breaking**: новый RPC и новые сообщения (номера полей с 1), существующие сообщения и номера не тронуты. Порядок выкладки: сначала ledger, затем BFF; старый ledger на этот метод ответит `UNIMPLEMENTED`, BFF обязан обработать.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc GetAgentPnl(GetAgentPnlRequest) returns (GetAgentPnlResponse);
```

## Caller

- frontend-api / BFF (`GET /api/vector-clearing/agent-pnl`)

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

```proto
message PnlGroup {
  bool has_data = 1;                 // false ⇒ r, u, r_plus_u НЕ заданы (нет цены — это не ноль)
  fob.common.v1.Decimal r = 2;       // USDT
  fob.common.v1.Decimal u = 3;       // USDT
  fob.common.v1.Decimal r_plus_u = 4; // USDT, r + u
}
message AgentPnl {
  string agent_id = 1;               // "T_BTC_binance" | "A_BTC_binancekraken"
  string agent_kind = 2;             // "translator" | "arbitrageur" | ""
  string asset = 3;                  // базовый актив
  string quote = 4;                  // котируемая валюта пары ("USDT" у пар к USDT и у связки)
  repeated string venues = 5;        // площадки ног: переводчик одна, связка две
  PnlGroup internal = 6;             // внутренняя биржа CE
  PnlGroup external = 7;             // внешние биржи, все вместе
  PnlGroup total = 8;                // internal + external
  int64 mark_age_ms = 9;             // возраст цены оценки m; -1 ⇒ цена неизвестна
  uint32 ext_fills = 10;             // число внешних хедж-исполнений с запуска ledger
  int64 since_ms = 11;               // начало накопления, unix ms; 0 ⇒ не начато
}
message AgentPnlTotals {
  PnlGroup internal = 1;             // сумма по агентам, у которых в этой группе есть данные
  PnlGroup external = 2;
  PnlGroup total = 3;
  uint32 agent_count = 4;            // сколько агентов в ответе
  uint32 incomplete_agents = 5;      // у скольких хотя бы в одной группе has_data = false
}
message GetAgentPnlRequest {
  fob.common.v1.EventMeta meta = 1;
  repeated string agent_ids = 2;     // пусто = все агенты
}
message GetAgentPnlResponse {
  fob.common.v1.EventMeta meta = 1;
  string currency = 2;               // "USDT"
  repeated AgentPnl agents = 3;
  AgentPnlTotals totals = 4;         // строка «все агенты»
  bool enabled = 5;                  // CE_AGENT_PNL_ENABLED
}
```

Примечания к контракту:

- **Единицы.** Все r, u, r_plus_u — USDT, `Decimal` (деньги — [.claude/rules/data.md](../../../.claude/rules/data.md)), масштаб до 8 знаков. Пересчёт из валюты котировки в USDT делает ledger (курс $\rho=P_b/p$ на момент выдачи).
- **`has_data`.** Нулевое число и «нет данных» различаются. `internal.has_data=false` — у агента нет цены оценки (ни одной дельты с ценой с момента запуска) либо учёт приостановлен (книги не загружены из PostgreSQL при старте, см. ADR-087 п. 9). `external.has_data=true` с нулями — цена известна, внешних исполнений не было (у связки так и должно быть по ADR-083). `total.has_data` истинно только если истинно в обеих группах: частичных сумм нет.
- **Итоги `totals`.** Суммы только по агентам, у которых в группе есть данные; `incomplete_agents` показывает неполноту. Итоги считаются по возвращённым агентам (при фильтре `agent_ids` — по выбранным).
- **`enabled=false`.** Ledger запущен с `CE_AGENT_PNL_ENABLED=0`: список агентов пуст, `totals` нулевые, `has_data=false`.
- **Идемпотентность.** RPC read-only, идемпотентен по конструкции — без `idempotency_key` (как `GetAgentPositions`). Данные берутся из памяти ledger под `mu_`; PostgreSQL на каждый вызов не читается. Сверка книги с позицией на чтении выполняется на копии и состояние не меняет.
- **Что не входит.** Комиссии не вычитаются (как в панели «Живые позиции»); история не отдаётся (график остаётся на `ce_agent_pnl_history`).

## Used In Features

- [F-18](../../02-system/features/F-18-ce-capital-position-hedge/feature.yaml) — AC-F18v2-30.

## Related

- Карточка расчёта: [CALC-CE-AGENT-PNL](../../04-domain/calculations/CALC-CE-AGENT-PNL.md).
- Хранение: [ce-agent-pnl-book](../../07-data/ce-agent-pnl-book.md), колонки в [ce-agent-position](../../07-data/ce-agent-position.md).
- Родственные методы: [GetAgentPositions](ledger-agent-positions.md).
