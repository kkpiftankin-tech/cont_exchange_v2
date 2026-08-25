---
source: runtime-investigation
authors: Alexander Piftankin + Claude
captured_at: 2026-05-22
trigger: >
  Диагностика "почему не исполняется FlowOrder" на ubuntu-dev deployment
  выявила три расхождения между runtime-поведением и спецификациями F-04/F-12.
runtime_env:
  host: ubuntu-dev (100.65.232.81)
  branch: sync/dev-import @ commit 1eca0757
  commit_date: 2026-05-22
---

# Runtime findings: order_flow ↔ matching ↔ ledger ↔ venues integration gaps

Документ фиксирует три бага, обнаруженных при ручной торговле с UI на ubuntu-dev.
Все три — разрывы цепочки docs/02-system/features/F-04 + F-12 в её текущей
реализации (skeleton MVP с реальным Eigen-солвером + симулятором venue-execution).

## Контекст эксперимента

1. Создан FlowOrder через UI / `POST /v1/flow-orders`:
   - order_id = `89b0a0aa-5bc5-485b-aa31-d967b63f8305`
   - side = sell, symbol = BTC/USDT
   - total_qty = 1 BTC
   - price_low = 76000, price_high = 77500
   - max_speed = 0.1
2. Matching service принял заявку, solver признал `feasible=true`,
   планировщик выбрал `best_venue=binance` / `uniswap_v3`, начал
   эмитировать `execution.intents`.
3. Через ~3 минуты в ClickHouse зафиксировано 70 fills для этого order_id
   с суммарным `executed_qty = 2.000002 BTC` и avg price 76434.10.

## Bug-1 — order_flow не подписан на `batch.outputs`

**Symptom.** После 70 fills в ClickHouse, gateway по-прежнему возвращает
FlowOrder в состоянии `status: "new"`, `remaining_qty: 1`. UI Progress
показывает 0%, хотя на бэке заявка фактически исполнена.

**Evidence.**

- `GET http://gateway:8088/v1/flow-orders?user_id=demo-user` — order
  остаётся `new` / `remaining_qty=1`.
- `docker logs infra-order_flow-1` — после старта ни одного INFO/DEBUG
  про consumption `batch.outputs` или `fills`.
- ClickHouse:
  ```sql
  SELECT count(), sum(executed_qty), avg(price) FROM fills
   WHERE order_id LIKE '89b0a0aa%';
  -- 70 | 2.000002 | 76434.10
  ```

**Root cause hypothesis.** В `cpp/order_flow` отсутствует Kafka consumer
для топика `batch.outputs` (или эквивалентный gRPC/event-feed от ledger).
В `feature.yaml` F-04 декларирован поток `matching → batch.outputs →
ledger.ApplyBatchResult → updates flow_orders`, но code path не
замыкается: ledger получает события, но не сообщает order_flow.

**Impact.** UI не показывает progress, любая зависимая логика
(cancellation после filled, post-trade reconciliation, profile activity
list) работает на неактуальном состоянии.

**Target docs.**

- `docs/02-system/features/F-04-batch-clearing/feature.yaml` → `knownIssues`
- Service sequence `docs/05-components/sequences/SEQ-F04-UC-F04-01-services.md`
  (нужна явная стрелка batch.outputs → order_flow или ledger → gRPC →
  order_flow).
- Code: `cpp/order_flow/src/app/order_flow_uc.cpp`, `cpp/order_flow/src/main.cpp`.

## Bug-2 — Overfill: matching отдаёт fills за пределами `total_qty`

**Symptom.** Order на 1 BTC исполнен на 2.000002 BTC в `fills` ClickHouse.
Solver продолжал генерировать fills даже после превышения total_qty.

**Evidence.**

- Matching log:
  ```
  active_after=1 (повторяется ~14 batch tick подряд, каждый fills_count=1)
  ...
  active_after=0  // только на последнем тике
  ```
- ClickHouse SUM(executed_qty) = 2.000002 для одного order_id.

**Root cause hypothesis.** Matching service хранит `active_orders` в памяти
и НЕ получает обратной связи от ledger о применённых fills. Solver на каждом
batch tick считает остаток заявки от своей внутренней копии, которая
правильно убывает, но:

- либо publish_batch дублирует BatchResult (одна запись fills → две строки
  в ClickHouse),
- либо matching действительно даёт fills больше чем `remaining_qty`
  (нет hard cap внутри solver).

Соотношение 70 fills vs ~34 batch ticks (171 сек / 5 сек) намекает на
дубликаты: 2 fills/batch.

**Impact.** Critical. В проде это означало бы:
- продажу BTC, которого нет на балансе (margin call, liquidation чужих средств);
- audit-trail с дублирующимися fills (нарушает Ledger invariant из CLAUDE.md §17 «не применять fill дважды»).

**Target docs.**

- `docs/02-system/features/F-04-batch-clearing/feature.yaml` → `knownIssues`
- Domain rule `docs/04-domain/business-rules.md` — добавить invariant:
  `SUM(fills[order_id].executed_qty) <= flow_orders.total_qty`.
- Code: `cpp/matching/src/app/matching_loop.cpp`, `cpp/matching/src/app/run_batch_uc.cpp`,
  `cpp/observability/` (consumer batch.outputs → ClickHouse).

## Bug-3 — Topic naming mismatch: `execution.venue` vs `execution.reports`

**Symptom.** В CLAUDE.md §7.3 и docs/06-api/messaging/* зафиксирован топик
`execution.reports`, по которому ledger должен получать отчёты об
исполнении от venues. На runtime `cpp/venues` публикует в **`execution.venue`**.

**Evidence.**

```
{
  "msg":"Produced execution report",
  "topic":"execution.venue",
  "service":"venues",
  "source_file":"cpp/venues/src/app/venues_loop.cpp",
  "stage":"publish_execution_report"
}
```

Ни одного сообщения в логах ledger о consume `execution.reports` за
последние 5 минут (хотя venues выпустил 70 reports).

**Root cause hypothesis.** В рамках ingest IN-005 (F-12) был импортирован
runtime-код от другой ветки с переименованным топиком `execution.venue`,
но docs/CLAUDE.md/proto-map не обновлены. Либо это сознательная
эволюция (split на venue-specific и aggregate stream), которая не
задокументирована.

**Impact.** Ledger не применяет execution-fills к hedge-балансу. Любой
auto-hedge сценарий из F-12-01 / F-12-05 будет молчаливо ломаться.

**Target docs.**

- `docs/02-system/features/F-12-execution-hedge/feature.yaml` → `knownIssues`
- `CLAUDE.md` §7.3 — добавить `execution.venue` рядом с `execution.reports`
  или зафиксировать ADR.
- `docs/06-api/messaging/execution-reports.md` — синхронизировать имя топика.
- `specs/contracts/proto-map.yaml` — venue-specific stream (если split).

## Recommendation

Завести fix-PR(ы) после мержа PR #12:

1. **PR-A: order_flow batch.outputs consumer + idempotent flow_orders update**
   - Подписать `cpp/order_flow` на `batch.outputs`.
   - На каждом fill — UPDATE `flow_orders.filled_cum`, пересчитать `status`
     (active / partially_filled / filled).
   - Атомарность через unique (batch_id, fill_idx) constraint.

2. **PR-B: Hard cap fills в matching solver**
   - Внутри `RunBatchUseCase::Execute` проверять перед publish:
     `sum(batch.fills[].executed_qty per order_id) <= active.remaining_qty`.
   - Если превышение — обрезать или skip-and-log.
   - Параллельно дебажить почему 70 ≠ 34 (возможно, наш observability consumer пишет дубликаты).

3. **PR-C: Topic name unification**
   - Либо переименовать `execution.venue` → `execution.reports` (recommended,
     соответствует CLAUDE.md / feature spec).
   - Либо создать ADR-015 «venue-specific execution stream `execution.venue`
     дополняет агрегированный `execution.reports`».

Каждый PR должен сопровождаться integration test в `cpp/matching/tests/domain/`
и `cpp/order_flow/tests/`, который воспроизводит исходный сценарий
(order 1 BTC → fills → assert UI state == backend state).
