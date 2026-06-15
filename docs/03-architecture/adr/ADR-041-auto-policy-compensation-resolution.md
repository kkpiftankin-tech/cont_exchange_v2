---
id: ADR-041
status: accepted
date: 2026-06-15
owners:
  - architecture
  - core-team
  - risk
related:
  - docs/03-architecture/adr/ADR-039-compensation-resolution.md
  - docs/03-architecture/adr/ADR-040-compensation-resolution-cross-service.md
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - cpp/order_flow/src/app/resolve_compensation_use_case.cpp
  - cpp/matching/src/infra/postgres_combo_compensation_repository.cpp
  - CLAUDE.md (§9 money, §16 risk, §17 ledger, §22 audit)
source: ADR-039 §1 (auto-policy deferred to MVP-7); F-09 MVP-7
---

# ADR-041: Auto-policy compensation resolution (MVP-7)

## Контекст

ADR-039 зафиксировал разрешение combo-компенсаций как **operator-driven** и
**отложил авто-policy в MVP-7** — «после накопления статистики и risk-review»,
потому что автоматический реверс денег — самая рискованная зона (CLAUDE.md §9/§17):
неконтролируемый каскад реверсов может усугубить потери (реверс по плохой цене,
петля реверсов). ADR-040 разместил operator-резолв в order_flow (тот же
`ResolveCompensation` путь). Финал ADR-039: «переход на авто-policy — **отдельный
слой над** operator gRPC». Этот ADR определяет такой слой **безопасно**.

## Решение

### 1. Auto-policy — аддитивный слой над operator-резолвом, тот же money-путь

Авто-резолв НЕ вводит новый денежный путь: он вызывает **тот же**
`ResolveCompensationUseCase` (ADR-040), что и оператор, с `operator_id =
"auto:<rule_id>"` для audit. Operator-консоль (slice 4) остаётся; авто берёт на себя
**только ограниченные/безопасные** случаи, всё остальное **оставляет pending**
оператору. Авто — помощник, не замена.

### 2. Авто-действие = только `reverse_internal`, иначе escalate

| Действие | Авто? |
| --- | --- |
| `reverse_internal` | **да** — если в пределах guardrails (разворачивает внутреннюю экспозицию) |
| `accept` | **никогда** — принять экспозицию = человеческое risk-решение |
| `retry_external` | **никогда** (авто) — остаётся manual/deferred |

Всё, что не проходит guardrails → **остаётся pending** (fail-safe; не авто-accept,
не авто-retry).

### 3. Money-guardrails (§9/§17) — авто-реверс срабатывает ТОЛЬКО при всех условиях

1. `reason ∈ {rejected, timeout, cancelled}` — терминальный провал внешней ноги.
2. notional реверса (оценка `Σ filled_cum · reference_price`) ≤
   `F09_AUTO_MAX_NOTIONAL` — потолок на одну компенсацию.
3. суммарный авто-реверс notional в скользящем окне ≤ `F09_AUTO_WINDOW_NOTIONAL`
   — **circuit-breaker против каскада** (ядро риска ADR-039).
4. число авто-резолвов в окне ≤ `F09_AUTO_MAX_PER_WINDOW`.
5. возраст компенсации ≥ `F09_AUTO_MIN_AGE_MS` — debounce: дать транзиентным
   venue-сбоям / поздним fill'ам осесть и не гоняться с оператором.
6. компенсация ещё `pending` (идемпотентность — gate `ResolvePending`).

Любое нарушение → pending → оператор. Лимиты — из env (как F-09 policy T-F09-002).

### 4. Kill-switch / enablement

Авто-policy gated на `F09_AUTO_RESOLVE_ENABLED` (**default OFF** — opt-in после
статистики/risk-review, ADR-039). Рантайм-выключение немедленно останавливает
авто-резолв; pending остаётся оператору. Circuit-breaker (§3.3) при срабатывании
окна тоже фактически приостанавливает авто до следующего окна.

### 5. Размещение — loop в order_flow (ADR-040)

Авто-resolve loop живёт в **order_flow** (владеет путём резолва). Периодически
(`F09_AUTO_RESOLVE_INTERVAL_MS`): `matching.CompensationService.ListPendingCompensations`
→ pure `AutoResolvePolicy(pending, config, now) → {auto_reverse | escalate}` →
для eligible вызывает `ResolveCompensationUseCase(action=reverse_internal,
operator_id="auto:<rule>")`. matching остаётся владельцем таблицы; никакого нового
money-пути. (Требует добавить `ListPending` в `MatchingCompensationClient`.)

### 6. Audit + детерминизм

Каждый авто-резолв: `operator_id="auto:<rule>"`, structured log + risk.alert
(`auto_compensation_resolved`), `resolving_ref` = id реверсивных FlowOrder — так же
прослеживаемо, как оператор (§22). Политика — **чистая функция**; gate
`ResolvePending` + детерминированный `order_id` реверса (slice 3b) делают повторную
оценку безопасной (нет двойного реверса). Объём реверса — `combo_order_legs.filled_cum`
(ADR-039 §3); сложная partial-математика НЕ меняется (что политика не может
ограничить → pending).

## Альтернативы

- **Авто-accept под policy** — отклонено: принятие экспозиции — человеческое решение.
- **Авто-каскад без оконного потолка** — отклонено: это ровно money-риск ADR-039.
- **Policy в matching** — отклонено: путь резолва — order_flow (ADR-040); matching
  остаётся владельцем таблицы и gate-резолва.
- **Авто-retry_external** — отклонено в MVP-7: cross-venue retry-роутинг — отдельный
  scope (ADR-039 deferred).

## Последствия

- (+) Снижает operator-toil для частого ограниченного случая; эскалирует крупное/рисковое.
- (+) Нет нового money-пути — переиспользует slice-3b; консоль остаётся.
- (−) Новый config-surface (guardrail-лимиты) + периодический loop + `ListPending` rpc.
- (−) Оконный учёт (notional/count) — MVP in-memory per order_flow-инстанс (single-instance
  допущение; multi-instance → вынести в PG, отдельная задача).

## Обратимость

Полностью обратимо: `F09_AUTO_RESOLVE_ENABLED=false` возвращает чистый
operator-driven режим (ADR-040). Слой аддитивен; откат — `git revert` без миграций
(данные/таблица не меняются).

## Открытые вопросы

- Reference price для оценки notional — переиспользовать market_data (как combo create).
- Оконный store — MVP in-memory; PG-вариант при multi-instance order_flow.
