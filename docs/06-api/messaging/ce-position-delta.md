# Kafka Topic: `ce.position.delta`

## Status

**Planned-contract (эволюция payload)** — proto-правка `AssetDelta → AgentDelta` относится к стадии кода F-18 v2 (T-F18-007..011; ADR-061). Топик уже создаётся в `infra/kafka/create_topics.sh`; в таблице `docs/06-api/messaging/topics.md` он на момент написания **не задокументирован** — добавить строку заодно при реализации.

**Реализовано инкрементально (2026-09-15, T-F18-007/201, за флагом `CE_AGENT_POS`, default OFF).** Отгружен **аддитивный** первый шаг вместо разового breaking-перехода: в существующий `AssetDelta` добавлены поля `agent_id = 6` и `agent_kind = 7` (enum `AgentKind {UNSPECIFIED, TRANSLATOR, ARBITRAGEUR}` в `treasury.proto`), поля 1–5 не тронуты. При `CE_AGENT_POS=1` matching эмитит по строке на агента (ребро графа), а поле `delta` несёт знаковый сырой поток такта `Δc_j = f_j` (направление агента), не количество. Раздел «Эволюция payload» ниже описывает конечную целевую форму (отдельное сообщение `CeAgentPositionDeltaBatch`/`AgentDelta`) — к ней переходим позднее; текущая отгрузка её подмножество, обратно-совместимое по wire. Флаг не включать в общих средах до ledger-накопления per-agent (T-F18-202) и `GetAgentPositions` (T-F18-203) — иначе consumer примет `f_j` за количество.

## Purpose

Поток дельт позиции CE между тактами клиринга: `matching` вычислил такт → публикует изменение позиции, `ledger` применяет его к источнику истины. В новой постановке (v2, [`incoming-docs/2026-09-15-CE_algorithm_v2.md`](../../../incoming-docs/2026-09-15-CE_algorithm_v2.md), A6) дельта — это `Δc_j = f_j` **на агента** (`c ← c + f`), а не проекция на holdings биржи.

## Producer

- matching (после `Solve` и проверки инвариантов — публикует дельты позиций агентов)

## Consumers

- ledger (единственный известный consumer; применяет upsert в [`ce_agent_position`](../../07-data/ce-agent-position.md) с guard по `last_batch_id`)

## Key / Retention

- Key: `agent_id` (v2) — все дельты одного агента идут в одну партицию, сохраняя порядок накопления `c ← c + f`. (v1 использовал ключ уровня house/asset.)
- Delivery: at-least-once + **идемпотентный consumer** — `ledger` дедуплицирует по `(agent_id, batch_id)` через guard `last_batch_id IS DISTINCT FROM ...` (CLAUDE.md §13, §17).
- Retention: аудит-класс (не market-data); согласовать со значением в `create_topics.sh`.

## Эволюция payload: `AssetDelta` → `AgentDelta`

### v1 (до) — `CePositionDeltaBatch` / `AssetDelta`

```proto
message AssetDelta {
  string asset = 1;
  fob.common.v1.Decimal delta = 2;
  string venue = 3;
  fob.common.v1.Decimal price_used = 4;
  fob.common.v1.Decimal delta_value = 5;
}
message CePositionDeltaBatch {
  fob.common.v1.EventMeta meta = 1;
  string batch_id = 2;
  int64 event_time_ms = 3;
  repeated AssetDelta deltas = 4;
}
```

Семантика v1: `Δ` **house**-позиции по активу (проекция клиринга на holdings биржи), питала `Z_a = nop + committed + in_transit − target` в risk.

### v2 (после) — `CeAgentPositionDeltaBatch` / `AgentDelta`

```proto
message AgentDelta {
  string agent_id = 1;                     // "Q_BIN" | "T_BTC" ...
  fob.common.v1.AgentKind agent_kind = 2;  // TRANSLATOR | ARBITRAGEUR
  string asset = 3;
  string venue = 4;                        // площадка узла; "" — route-level (арбитражёр, V=2)
  fob.common.v1.Decimal delta = 5;         // Δc_j = f_j этого такта (A6: c←c+f), ЗНАКОВАЯ
  fob.common.v1.Decimal price_used = 6;    // диагностика
  fob.common.v1.Decimal delta_value = 7;   // диагностика
}
message CeAgentPositionDeltaBatch {
  fob.common.v1.EventMeta meta = 1;
  string batch_id = 2;
  int64 event_time_ms = 3;
  repeated AgentDelta agent_deltas = 4;
}
```

**Почему это breaking (семантически), даже при additive wire-формате.** Меняется экономический смысл: house **перестаёт** копить позицию по активу вообще (v2: прибыль падает на «счёт дома» — невязку, позиция — только у агента). Consumer, интерпретирующий данные по-старому (`Z_a` через house-дельту), получит неверный результат, даже если Kafka-схема формально совместима. Прямое попадание в CLAUDE.md §3.3 (модель денежных расчётов / clearing algorithm) → **ADR-061 обязателен**.

**Миграция.** Топик internal-only, единственный consumer — `ledger`. Переключить producer (`matching`) и consumer (`ledger`) на новый payload синхронно в одном релизе (без dual-publish периода). Старые `AssetDelta` / `CePositionDeltaBatch` — не переиспользовать номера полей, пометить deprecated, вывести из proto отдельной чисткой (sunset-план в ADR-061).

### ADR-072 — `fair_price` (F_0), аддитивно к `AssetDelta`

Поле 10, добавлено для прибыли переводчика ([ADR-072](../../03-architecture/adr/ADR-072-ce-translator-profit-hedge-price-skew-resting.md) §A):

```proto
  fob.common.v1.Decimal fair_price = 10;  // F_0 = P(base)/P(quote) ДО inventory-skew
```

- **Смысл.** `price_used` (поле 4) на per-agent пути несёт **сдвинутую** цену пары (skew вшит) = прежний лимит хеджа. `fair_price` несёт **справедливую** цену пары `F_0` ДО skew (единицы — цена пары, не USDT). Ledger строит клэмпленный лимит `L_sell=F_0+max(0,m−γn⁺)`, `L_buy=F_0−max(0,m−γn⁻)` — сдвиг остаётся, но не уходит за границу прибыли `F_0`. Запас `m=ρ·h_v` (ρ — env `CE_HEDGE_MARGIN_RHO`); `h_v` ledger берёт из своего источника спреда/глубины (`venue.liquidity.fob`/`venue.snapshots`), в контракт не кладётся.
- **Совместимость / фолбэк.** Аддитивное optional поле, не breaking. Пусто (`unset`/0) при `CE_AGENT_POS=0`, у старого matching и у арбитражёра ⇒ ledger откатывается на `price_used` (прежнее поведение). Порядок деплоя любой; новую логику лимита в ledger включать, только когда matching эмиттит `fair_price`.

## Live-bug (контекст, чинится в v2)

В текущей v1-реализации consumer **отбрасывает** `AssetDelta.venue`: дельта агрегируется по `asset` без учёта площадки (`cpp/.../kafka_consumers.cpp:62`). Из-за этого позиция «размазана» по активу, а не по узлу «актив@площадка». v2 это исправляет: `AgentDelta` несёт `agent_id` + `venue`, а `ce_agent_position` имеет PK `(agent_id, asset, venue)` — площадка больше не теряется на приёме.

## Decimal / EventMeta

- `delta`, `price_used`, `delta_value`, `base_price`, `fair_price` — `fob.common.v1.Decimal` (деньги/объём/цена, CLAUDE.md §9).
- `CeAgentPositionDeltaBatch.meta` — `EventMeta` присутствует (обязателен для публикуемого в Kafka сообщения; несёт `correlation_id`, `partition_key`).

## Used In

- Feature: F-18 v2 (CE-агенты v2, виртуальные контрагенты)
- Data object: [`ce_agent_position`](../../07-data/ce-agent-position.md)
- RPC-чтение позиции: [ledger-agent-positions.md](../grpc/ledger-agent-positions.md)

## Related ADR

- ADR-061 (TODO) — модель позиции агента v2; supersedes ADR-057/058.
