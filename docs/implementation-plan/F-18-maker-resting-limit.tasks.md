# Implementation Tasks: F-18 #5 — мейкер-зона band-хеджа как резидентный лимит (ADR-067)

> **Тип изменения:** `MVP implementation` (hedge-execution policy, ADR-067). Кросс-сервис:
> risk (TIF_GTC + эскалация) → venues (resting-lifecycle) → ledger (clim-fee, A7 без изменений).
> **Основание:** [ADR-067](../03-architecture/adr/ADR-067-ce-maker-resting-limit-hedge.md),
> Кривые §6.4, domain-review `trading-domain-specialist`. **За флагом** `CE_BAND_MAKER_RESERVE_ENABLED`
> (default off). **Дата:** 2026-09-28. **Ветка:** `feat/F-18-ce-treasury-nop`.

## Цель

Мейкер-зона (`Z̄lim<|z|≤Z̄mkt`) эмитит резидентный лимит (`EXEC_STRATEGY_LIMIT`+`TIF_GTC`), который
стоит несколько тактов, накапливает partial fills до цели, платит мейкер-комиссию `clim`, эскалирует
в тейкер по таймауту. Тейкер-зона (`|z|>Z̄mkt`) без изменений. За флагом (default off).

## Задачи

### T-F18-801 — Docs-first (домен + данные + приёмка + тесты)
**Приоритет:** Высокий (гейт No-code-before-docs). **Файлы:** `docs/04-domain/business-rules.md`
(трёхзонное правило — первичный, сейчас отсутствует), `docs/07-data/hedgeflows.md` (band = OPEN-сессия
многотактовая), `docs/02-system/features/F-18-*/acceptance-criteria.md` (критерий мейкер-резерва),
`docs/10-testing/` (тест-план мейкер-lifecycle).
- **Приёмка:** трёхзонное правило + мейкер-резидентность описаны как источник истины; ссылки на ADR-067.

### T-F18-802 — risk: TIF_GTC для мейкер-зоны + эскалация в тейкер
✅ **СДЕЛАНО** (`73fc653d`): мейкер-зона → TIF_GTC + timeout_ms=CE_BAND_MAKER_DEADLINE_MS за флагом; тейкер/off → IOC. Эскалация — через новый breach-цикл risk после EXPIRED (venues не эскалирует сам).
**Приоритет:** Высокий. **Файл:** `cpp/risk/src/app/risk_uc.cpp` (`EmitBandHedgeFromBreach`).
- За флагом `CE_BAND_MAKER_RESERVE_ENABLED`: `aggressive=false` → `EXEC_STRATEGY_LIMIT`+`TIF_GTC`,
  дедлайн `CE_BAND_MAKER_DEADLINE_MS`. По таймауту (или уже `aggressive`) → `MARKET`+`IOC`.
- **Приёмка:** мейкер-пробой эмитит GTC-лимит; тейкер-пробой — market (как раньше). Флаг off = всё IOC.

### T-F18-803 — venues: resting-lifecycle лимита (основной объём)
✅ **СДЕЛАНО** (`6afc9622`): resting_band_orders_ + progress_resting_orders_locked (доматч на тиках RequestSnapshot, кумулятивный filled, стабильный venue_order_id → no-double-fill), порт ExecutionReportSink (VenuesLoop, единый PublishExecutionReport), price-impact gate на marketable. code-review money-path CLEAN; blocking-фикс (публикация follow-up ВНЕ mu_) применён. cpp-service-architect дизайн.
**Приоритет:** Высокий. **Файл:** `cpp/venues/src/infra/cex_ws_rest_adapter.cpp`.
- GTC LIMIT НЕ терминировать за один read-цикл: держать resting-заявку (per intent: цена/сторона,
  накопленный `filled_qty`, дедлайн), на каждом цикле доматчивать против НОВЫХ сделок (`crosses_price`);
  `PARTIALLY_FILLED` по инкременту, `FILLED`/`EXPIRED` по цели/дедлайну.
- **Багфикс:** price-impact начислять только для `marketable` (тейкер), не для пассивного лимита.
- **Приёмка:** мейкер-лимит живёт >1 такта, копит fill; пассивный fill без price-impact; тейкер как раньше.

### T-F18-804 — ledger: мейкер-комиссия clim на fill (общее с #6)
↪️ **СВЕДЕНО В #6** (тот же maker/taker-fee-on-fill в hedge PnL) — 803 работает без него (fee — уточнение PnL, не требование resting). Реализуется в рамках backlog #6.
**Приоритет:** Средний. **Файл:** `cpp/ledger/src/app/ledger_uc.cpp`.
- В `apply_agent_band_report_locked`: fee = `clim` для мейкер-зоны / `cmkt` для тейкер — зафиксировать
  на эмиссии в `band_hedges_` (live-реконфиг не двигает комиссию в полёте). A7 без изменений.
- **Приёмка:** мейкер-fill платит clim, тейкер — cmkt; отражено в hedge PnL / диагностике. (Смыкается с #6.)

### T-F18-805 — деплой за флагом off + проверка
🟡 **SMOKE-ПРОВЕРЕНО (2026-09-28), permanent-включение = owner-gate.** Собрано (risk+venues+ledger), флаг off → регресс чист (tif=IOC, 0 resting follow-up, цепочка здорова). Smoke `CE_BAND_MAKER_RESERVE_ENABLED=1` на dev: risk tif=GTC (мейкер) / IOC (тейкер); venues 5 resting follow-up fills (status FILLED, кумулятивный filled растёт); ledger 0 double-fill/regression/ошибок. Флаг возвращён в off. Permanent-включение на dev/staging — owner sign-off (как T-F18-705).
**Приоритет:** Высокий.
- Собрать risk+venues+ledger; флаг off → поведение не меняется (регресс-чек). Затем на dev включить
  `CE_BAND_MAKER_RESERVE_ENABLED=1` (owner-gate) и проверить: мейкер-лимит стоит несколько тактов,
  копит fill, эскалирует по таймауту; тейкер как раньше; lag 0.

## Порядок

801 (docs) → 802 (risk) → 803 (venues, осн.) → 804 (ledger fee) → 805 (деплой+проверка).
Флаг off до 805 — прод-поведение не меняется.

## Маршрутизация (agent-routing)

Класс L (hedge-execution policy, ADR). Триггеры §2: `code-reviewer` (money-path: ledger/risk/venues),
реализация через `code-implementer` (worktree). Домен-дизайн — `trading-domain-specialist` (выполнен).
