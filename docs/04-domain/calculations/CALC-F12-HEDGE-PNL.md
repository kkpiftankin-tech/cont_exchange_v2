---
id: CALC-F12-HEDGE-PNL
title: Hedge PnL (результат внешнего хеджа)
level: L3
feature: F-12
status: drift # три места расчёта/хранения не вполне согласованы — см. §7
code:
  - cpp/ledger/src/app/ledger_uc.cpp#calculate_hedge_pnl
  - cpp/ledger/src/app/ledger_uc.cpp#RecordHedgeExecution
  - cpp/market_data/src/app/hedge_pnl_aggregator.cpp#HedgePnLAggregator (агрегат для ClickHouse/дашборда, отдельный путь)
tests:
  - cpp/ledger/tests/ledger_hedge_pnl_test.cpp
  - frontend/web/src/api/__tests__/hedgeFlowService.pnl.test.js
ui:
  - route: /hedge-flows-live
    component: frontend/web/src/pages/HedgeFlowMonitor/HedgeFlowMonitorLive.js
    field: hedgePnl (per-row), cumulativePnl
  - route: /hedge-pnl
    component: frontend/web/src/api/hedgeFlowService.js#getHedgePnlDashboard
    field: summary.totalPnl / summary.netAfterFees
sources:
  - docs/04-domain/business-rules.md#hedgepnl
  - infra/postgres/init.sql (таблица hedgeflows, колонка hedge_pnl — "computed by Settlement Ledger")
updated_at: 2026-09-28
---

# Hedge PnL

## 1. Смысл

Насколько внешнее исполнение хеджа на venue оказалось лучше или хуже
внутренней опорной цены сделки с клиентом. Положительное значение — хедж
исполнен выгоднее внутренней цены (позитивный capture); отрицательное —
хедж обошёлся дороже, чем внутренняя цена клиента.

## 2. Формула (как в коде `calculate_hedge_pnl`, `cpp/ledger/src/app/ledger_uc.cpp:2141`)

$$
\text{hedgePnL} = s \cdot (p_{\text{exec}} - p_{\text{int}}) \cdot q,
\qquad s = +1 \text{ для SELL},\; s = -1 \text{ для BUY}
$$

(в коде это не отдельный множитель `s`, а явный `if (side == SIDE_BUY) pnl.units = -pnl.units;` —
математически эквивалентно.)

| Обозначение | Смысл | Единицы | Откуда берётся (поле / функция) |
|---|---|---|---|
| p_exec | цена исполнения на внешней площадке | quote за единицу base | `HedgeExecution.executed_price` (proto), параметр `executed_price` |
| p_int | внутренняя опорная цена на момент клиентской сделки | quote за единицу base | `HedgeExecution.internal_price` (proto), параметр `internal_price` |
| q | исполненный объём хеджа | base | `HedgeExecution.executed_qty`, параметр `qty` |
| s | знак стороны исполнения на venue | — | `HedgeExecution.side` |

Тип везде `cex::common::Decimal` (CLAUDE.md §9) — `double` в этом пути не участвует.

## 3. Числовой пример (из `ledger_hedge_pnl_test.cpp::test_hedge_pnl_sell_profit`)

SELL на venue: executed_price = 60000, internal_price = 50000, qty = 0.1 BTC.

$$
\text{hedgePnL} = +1 \cdot (60000 - 50000) \cdot 0.1 = 1000
$$

Комиссии в эту формулу **не входят** — см. §7.1.

## 4. Инварианты и граничные случаи

- Decimal на всём пути ledger; при `qty = 0` результат 0.
- Знак зависит только от `side` и разности цен, не от знака самих цен.
- Идемпотентность: `RecordHedgeExecution` проверяет `hedge_id` в
  `hedge_pnl_records_[venue]` перед повторной записью (см. код, строки ~2451).
- `hedgeflows.hedge_pnl` в PostgreSQL — источник истины для UI-дашборда;
  комментарий в `infra/postgres/init.sql:143` называет её "computed by
  Settlement Ledger", что соответствует §5.

## 5. Путь числа

`HedgeFlowMonitorLive.js` → `frontend/web/src/api/hedgeFlowService.js`
(`normalizeHedgePnlRow`, поле `hedgePnl`/`hedge_pnl`) → `GET /hedge-pnl` в
`frontend/api/server.js` (SQL: `SELECT ..., hedge_pnl::text AS hedge_pnl, ...
FROM hedgeflows ...`) ← колонка `hedgeflows.hedge_pnl` ← записана
`LedgerUseCases::apply_execution_report_locked` /
`LedgerUseCases::RecordHedgeExecution` через `calculate_hedge_pnl` (ledger).

Отдельно, для агрегатов ClickHouse/операторского дашборда, существует ВТОРОЙ
путь: `ExecutionReport.hedge_pnl` (уже посчитанный ledger'ом,
protobuf `Decimal`) → `cpp/market_data/src/app/hedge_pnl_aggregator.cpp`
конвертирует в `double` (`decimal_to_double`) и суммирует в `double total_pnl`
для строки `hedge_pnl_agg`/ClickHouse. Это не пересчёт формулы, а
downstream-агрегация уже готового значения — но именно на этом шаге денежная
величина впервые становится `double` на своём пути от сделки до дашборда.

**Известное эксплуатационное ограничение** (см. `frontend/api/server.js`,
комментарий у `getHedgePnLFromLedger`/`HEDGE_PNL_PERIODS`): в dev
sim-режиме `hedge_pnl`/`avg_price` в ClickHouse-пути остаются 0
(`knownIssue: venues-sim-zero-execution-price`) — объёмы и комиссии при этом
реалистичны, только PnL-колонки нулевые. Это не баг формулы, а известное
состояние симулятора; при разборе замечания «PnL всегда 0» на дев-стенде
сначала проверь этот known issue, а не формулу.

## 6. Проверка

`ledger_hedge_pnl_test.cpp` содержит SELL/BUY кейсы с profit/loss (см. §3).
Формула воспроизводится вручную по §2 без запуска сервиса.

## 7. Открытые вопросы и расхождения

1. **Комиссии.** `docs/04-domain/business-rules.md` (раздел «HedgePnL») до
   этой правки описывал формулу с вычитанием `feesTotal`:
   `hedgePnL = (avgFillPrice − referenceMid) × filledQty − feesTotal`.
   Реализация `calculate_hedge_pnl` комиссий **не вычитает** — она считает
   валовой (gross) результат хеджа; комиссии учитываются отдельно (см.
   `ledger_uc.cpp` вокруг строк 1230–1242, `pos.realised_pnl -= fee_amount`
   для позиции клиента, а не для `hedge_pnl`). Значит на экране
   `/hedge-flows-live` поле `hedgePnl` — это gross-хедж-PnL, а
   `summary.netAfterFees` на `/hedge-pnl` — отдельно посчитанная net-метрика
   (`net_after_fees` из SQL-агрегата, а не из `calculate_hedge_pnl`). Это
   похоже не на баг, а на две разные, законные метрики под похожими
   названиями — но `business-rules.md` называл только net-формулу и
   приписывал её той же функции. Обновлено ниже (§ Conflict Note в
   `business-rules.md`); финальное решение — за `trading-domain-specialist`.

   **РЕШЕНО (2026-09-28, F-18 #6, trading-domain-specialist):** `calculate_hedge_pnl`
   ОСТАЁТСЯ gross (тесты `ledger_hedge_pnl_test` не ломаются). Комиссия учитывается
   ДВУМЯ раздельными величинами, НЕ смешиваемыми:
   - **`tot_fee`** — РЕАЛЬНАЯ комиссия venue (`report.fee_total()`), сейчас всегда
     taker (в venues нет концепции maker-ставки даже после T-F18-803). `netAfterFees`
     на `/hedge-pnl` = `hedge_pnl − tot_fee`.
   - **`band_fee_estimated`** (F-18 #6, T-F18-804) — РАСЧЁТНАЯ комиссия зоны band-хеджа:
     `Σ |Δq·P|·bps/1e4`, где `bps` = снимок `clim` (мейкер, `EXEC_STRATEGY_LIMIT`) или
     `cmkt` (тейкер) на момент ЭМИССИИ (`AgentBandHedge.fee_bps_snapshot`,
     `ledger_uc.cpp register_band_hedge_locked`/`apply_agent_band_report_locked`). Нужна
     потому, что для мейкер-зоны реальная `tot_fee` (taker) занижает/искажает издержку.
   Net band-хеджа = `gross − band_fee_estimated`. НЕ путать `band_fee_estimated` (расчётная,
   ledger-config) с `tot_fee` (реальная, venue) — разные поля, разный источник.
   Пропагация `band_fee_estimated` в PG (`hedgeflows.band_fee_estimated`) + BFF
   (`bandFeeEstimated`) + фронт — **СДЕЛАНО** (F-18 #6, `4e38f4e7`/`e26ff047`/`3af876d6`).
   ClickHouse-агрегат — осознанно отложен. Реальная maker-fee в venues — смежный backlog.
   - **`house_realized_pnl`** (F-18 #8, ADR-068, **observation-only**) — ТРЕТЬЯ независимая
     величина: признание прибыли по факту расчёта (§A8.2 CE_algorithm_v2):
     `Σ fq·(марка − px_факт)/1000·sgn`, где **марка = mid клиринга** (`clear price` символа,
     кэш `last_clear_price_` из `BatchResult`), `px_факт` = `average_price` отчёта, `sgn` —
     знак сокращаемой позиции (**провизорный** — открытый вопрос ADR-068). Разрыв план/факт
     `plan_fact_gap = Σ fq·(марка − px_факт)/1000` — отдельная величина «качество исполнения».
     Считается в `apply_agent_band_report_locked`, публикуется в PG (`hedgeflows.house_realized_pnl`
     /`plan_fact_gap`) + BFF (`houseRealizedPnl`/`planFactGap`) + фронт. **Баланс `__ce_house__`
     НЕ двигается** (observation-only); фактическое кредитование дома — owner-gated шаг после
     разрешения знака `sgn`. Отдельно от `hedge_pnl` (к `internal_price`) и `band_fee_estimated`
     (комиссия) — три независимые величины. Тест `ce_band_house_realized_test`.
2. **Именование опорной цены.** Три имени для одной величины: `internal_price`
   (код/proto), `referenceMid` (`business-rules.md`), `reference_mid`
   (колонка PostgreSQL). Не переименовывать без ADR — просто держать в уме
   при поиске по коду/докам одновременно все три написания.
3. **Статус реализации в `business-rules.md`.** Документ утверждал
   "реализация: планируется" — неверно, `calculate_hedge_pnl` и
   `RecordHedgeExecution` реализованы и покрыты тестами. Исправлено при
   заведении этой карточки.
4. **`double` в `hedge_pnl_aggregator.cpp`.** Формально это ClickHouse-агрегат
   (аналитика/история), для которого CLAUDE.md §9 разрешает `double`
   ("временных research/simulation calculations, если результат не попадает
   в ledger") — но здесь результат попадает в операторский дашборд PnL, а не
   только в research. Не переквалифицировано как нарушение без решения
   `trading-domain-specialist`/`code-reviewer`; зафиксировано как вопрос для
   следующего затрагивающего эту карточку изменения (триггер "деньги" в
   `.claude/rules/agent-routing.md` сработает автоматически при правке этого
   файла).
