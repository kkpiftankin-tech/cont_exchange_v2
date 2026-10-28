# gRPC Method: LedgerService/GetAgentPnl

## Status

**Accepted** (ADR-087, модель C′, 2026-10-28). Метод добавлен в `contracts/proto/fob/ledger/v1/ledger.proto` аддитивно. Смысл поля `PnlGroup.has_data` изменён в той же ветке, пока метод был только на dev: раньше «R и U известны», теперь «R известен», а известность U отдельно в новом поле `u_has_data = 5` (отдельный ADR не нужен, зафиксировано в ADR-087).

## Purpose

Вернуть **заработок агентов CE** (переводчиков и связок) в USDT, уже посчитанный в ledger: по каждому агенту три группы чисел — внутренняя биржа CE, внешние биржи, итого; в каждой R (реализованный результат), U (нереализованный) и R+U. Формулы, единицы и примеры — [CALC-CE-AGENT-PNL](../../04-domain/calculations/CALC-CE-AGENT-PNL.md), решение — [ADR-087](../../03-architecture/adr/ADR-087-ce-agent-pnl-internal-external-total.md). Нужен:

- **frontend-api / BFF** — вкладка «PnL» (`/hedge-pnl-live`): таблица «Заработок агентов». BFF пересылает ответ в JSON и арифметики над R и U не делает ([REST](../rest/agent-pnl.md)).

Read-only, additive к `LedgerService` — **не breaking**: новый RPC и новые сообщения (номера полей с 1), существующие сообщения и номера не тронуты. Порядок выкладки: сначала миграция БД, затем ledger, затем BFF (с пересборкой образа: копия proto запечена в образ BFF, иначе `u_has_data` отбрасывается при разборе и U пропадает у всех), затем web — BFF и web подряд. Старый ledger на этот метод ответит `UNIMPLEMENTED`, BFF обязан обработать. Новый BFF со старым ledger показывает R без U (`u_has_data=false`), неверных чисел нет; откат одного ledger безопасен.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc GetAgentPnl(GetAgentPnlRequest) returns (GetAgentPnlResponse);
```

## Caller

- frontend-api / BFF: `GET /api/vector-clearing/agent-pnl` (вкладка «PnL»), а также `GET /api/vector-clearing/agent-positions` и сборка строк панели «Живые позиции» и ряда для графика (`loadLiveAgentRows`) — один источник R и U (ADR-087 п. 10)

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

```proto
message PnlGroup {
  bool has_data = 1;                 // false ⇒ r НЕ задан (нет данных — это не ноль)
  fob.common.v1.Decimal r = 2;       // USDT
  fob.common.v1.Decimal u = 3;       // USDT; задан при u_has_data
  fob.common.v1.Decimal r_plus_u = 4; // USDT, r + u; задан при has_data и u_has_data
  bool u_has_data = 5;               // false ⇒ u и r_plus_u НЕ заданы (остаток есть, текущей справедливой цены пока нет)
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
  PnlGroup internal = 1;             // R — сумма по агентам с известным R, U — по агентам с известным U
  PnlGroup external = 2;
  PnlGroup total = 3;                // r_plus_u задан только при incomplete_agents = 0
  uint32 agent_count = 4;            // сколько агентов в ответе
  uint32 incomplete_agents = 5;      // у скольких R или U неизвестны
}
message KindPnlTotals {
  string agent_kind = 1;             // "translator" | "arbitrageur"
  PnlGroup internal = 2;             // R — сумма по агентам вида с известным R, U — с известным U
  PnlGroup external = 3;
  PnlGroup total = 4;                // r_plus_u задан только при incomplete_agents = 0
  uint32 agent_count = 5;
  uint32 incomplete_agents = 6;
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
  repeated KindPnlTotals by_kind = 6; // итоги по видам агентов (панель «Живые позиции»)
}
```

Примечания к контракту:

- **Единицы.** Все r, u, r_plus_u — USDT, `Decimal` (деньги — [.claude/rules/data.md](../../../.claude/rules/data.md)), масштаб до 8 знаков. Пересчёт из валюты котировки в USDT делает ledger (курс $\rho=P_b/F_0$ на момент выдачи; для пар к USDT $\rho=1$).
- **`has_data` и `u_has_data`.** Нулевое число и «нет данных» различаются, R и U известны независимо. `has_data=false` — R неизвестен: учёт приостановлен (книги не загружены из PostgreSQL при старте, ADR-087 п. 8) либо у переводчика пары не к USDT пока нет курса валюты котировки (цена базы и цена пары приходят с первой дельтой после перезапуска). `u_has_data=false` при `has_data=true` — R известен (книга сохранена в PostgreSQL, перезапуск его не стёр), а U нет: остаток позиции есть, текущей справедливой цены пока нет. Остатка нет — U равен нулю и известен без цены. `r_plus_u` задан только при обоих признаках. `total` в агенте: `has_data` истинно, если R известен в обеих группах, `u_has_data` — если U известен в обеих; у внутренней группы `u` всегда 0 (на внутренней бирже всё реализуется в такте клиринга).
- **Итоги по видам `by_kind`.** Те же суммы, но отдельно для переводчиков и арбитражёров (по записи на вид, у которого есть агенты). Нужны панели «Живые позиции»: её строки «переводчики», «арбитражёры» и «вместе» берутся отсюда и из `totals`, браузер и BFF их не складывают.
- **Итоги `totals`.** R суммируется по агентам с известным R, U — по агентам с известным U; `incomplete_agents` — сколько агентов с неизвестной частью. Пока есть неполные агенты, `r_plus_u` итогов не отдаётся (слагаемые из разных выборок). Итоги считаются по возвращённым агентам (при фильтре `agent_ids` — по выбранным).
- **`enabled=false`.** Ledger запущен с `CE_AGENT_PNL_ENABLED=0`: список агентов пуст, `totals` нулевые, `has_data=false` и `u_has_data=false`.
- **Идемпотентность.** RPC read-only, идемпотентен по конструкции — без `idempotency_key` (как `GetAgentPositions`). Данные берутся из памяти ledger под `mu_`; PostgreSQL на каждый вызов не читается. Вызов состояние книг не меняет.
- **Что не входит.** Комиссии не вычитаются (как в панели «Живые позиции»); история не отдаётся (график остаётся на `ce_agent_pnl_history`).

## Used In Features

- [F-18](../../02-system/features/F-18-ce-capital-position-hedge/feature.yaml) — AC-F18v2-30.

## Related

- Карточка расчёта: [CALC-CE-AGENT-PNL](../../04-domain/calculations/CALC-CE-AGENT-PNL.md).
- Хранение: [ce-agent-pnl-book](../../07-data/ce-agent-pnl-book.md), колонки в [ce-agent-position](../../07-data/ce-agent-position.md).
- Родственные методы: [GetAgentPositions](ledger-agent-positions.md).
