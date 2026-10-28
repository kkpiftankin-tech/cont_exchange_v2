# Changelog

Формат основан на [Keep a Changelog](https://keepachangelog.com/ru/1.0.0/).
Записи — по фичам (`F-XX`) и крупным изменениям. Подробности — в git-истории и PR.

## [Unreleased]

### F-18 — Заработок агентов по биржам (ADR-087, CR-F18-002, CR-F18-003, CR-F18-004, CR-F18-005, CR-F18-006, CR-F18-007, CR-F18-008, CR-F18-009, CR-F18-010)

- График «Сделки на внешних биржах по парам»: графики для конкретного агента. Клик по строке таблицы «Заработок агентов» выбирает агента сразу для графика «Заработок во времени» и для блока пар; в блоке виден значок агента и кнопка «все агенты». Маршрут `pair-trades-history` получил необязательный параметр `agent` (фильтр по сегменту агента в `hedge_flow_id`) и поле ответа `agent`; список пар и дата последней сделки строятся по хеджам этого агента, выбор пар запоминается отдельно для каждого агента. Агент без хеджей — пустой график с пояснением. Добавлен предел одновременных тяжёлых запросов (503 `pair_trades_busy`). Формулы и числа графика не менялись.

- График «Сделки на внешних биржах по парам»: в списке чекбоксов показаны все пары, по которым были сделки за 7 суток (новое поле ответа `known_pairs`), и по умолчанию отмечены все пары со сделками в окне, а не одна. Пара без сделок в окне неактивна и подписана датой последней сделки, добавлены кнопки «все» и «снять». Раньше пары без сделок в окне (кросс-пары SOL/BTC, ETH/BTC) пропадали из списка, а график показывался только по одной паре. Поле аддитивное; числа графика не менялись.

- График «Сделки на внешних биржах по парам»: второй, независимый чекбокс «− комиссия с цены продажи» — линия продажи показывает цену за вычетом расчётной комиссии (по тем же ставкам и правилу maker/taker, что и надбавка к покупке). Считает BFF (`sell_price_fee`, `sell_fee`), комиссия — только отображение графика и в заработок (R, U), ledger и панель не входит.

- График «Сделки на внешних биржах по парам»: чекбокс «+ комиссия к цене покупки» — линия покупки показывает цену с расчётной комиссией (стоимость каждой заявки × ставка maker для лимитных или taker для рыночных, ставки экрана «Клиринг»). Считает BFF (`buy_price_fee`, `fee` в ответе), фронт подставляет готовое поле; реальная комиссия бирж в сделках не записана, цена продажи не меняется.

- Вкладка «PnL»: блок «Сделки на внешних биржах по парам» — для каждой пары (чекбоксы) график с двумя осями: накопленный объём базовой валюты и цены покупки и продажи на внешних биржах во второй валюте пары (факт исполнения хедж-заявок агентов CE, все биржи вместе). Всё считает BFF (`GET /api/vector-clearing/pair-trades-history`), фронт только рисует; режим объёма — общий, нетто или раздельно.

- Вкладка «PnL»: график «Заработок во времени» — R, U и R+U по охвату (все агенты, переводчики, арбитражёры или один агент по клику на строку таблицы) и группе (итого, внутренняя биржа, внешние биржи), окна 1 ч…7 дн. BFF раз в минуту пишет итоги ledger (`ce_pnl_totals_history`) и внутреннюю группу агента (колонки `ce_agent_pnl_history`); график читает записанное (`GET /api/vector-clearing/pnl-history`), история копится с выкладки.

- Вкладка «PnL» (`/hedge-pnl-live`): таблица «Заработок агентов» — по каждому агенту R, U и R+U внутренней биржи CE, внешних бирж и итого (USDT, без комиссий). Все числа считает ledger (`GetAgentPnl`), BFF и фронт только показывают.
- Модель C′ (определения утверждены владельцем 2026-10-28): на внутренней бирже всё реализуется в такте клиринга — R = Σ (F_0 − p)·dL, U всегда 0, сумма R по агентам равна нулю (проверка в тесте и диагностика такта в журнале ledger); позиция хранится по справедливой стоимости входа, на внешних биржах R — качество исполнения хеджа плюс дрейф справедливой цены закрытой части, U — незахеджированный остаток по текущей справедливой цене; R+U равно результату агента по кассе.
- Прежняя редакция (учёт по средней себестоимости с внутренним U) заменена: накопление начинается заново с выкладки, ряд `ce_agent_pnl_history` до неё записан по прежним формулам (разрыв на графике). Миграция `ce_agent_pnl_book` (переименование колонок, очистка строк модели A) идемпотентна; колонки `ext_*`, `rvf_base` таблицы `ce_agent_position` больше не используются.
- Контракт: `PnlGroup.u_has_data` (R и U известны независимо — R сразу после перезапуска ledger, U после первой цены); `r_plus_u` итогов не отдаётся при неполных агентах. Порядок выкладки: ledger, затем frontend-api (с пересборкой образа), затем frontend-web.
- Панель «Живые позиции», ряд для графика и вкладка «PnL» берут R, U, R+U и итоги из ledger (`GetAgentPnl`, итоги по видам `by_kind`); формул R и U в BFF нет.

### F-09 — Batch / Combo / Multi-leg Orders (PR #13)

**MVP-1 (`orchestration_only`):**

- Контракты `combo.proto` / `execution_group.proto` + 5 rpc `OrderFlowService`
  (Create/CreateBatch/Preview/Cancel/Get combo); Kafka-топики `execution.groups`
  (+ `backtest.execution.groups`); 7 PG + 5 ClickHouse таблиц.
- order_flow: domain `ComboOrder` + инварианты (ADR-031/032/033), Postgres
  combo-репозиторий (атомарный insert 5 таблиц, идемпотентность), grouped
  producer (`orders.normalized`, backward-compatible), Create/Cancel use cases,
  gRPC-хендлеры. Проверено e2e (live) + 5 unit-сьютов.

**MVP-2 — grouped vector solver:**

- `cex::common::Decimal::div` (fixed-point деление, half-up) + unit-тест.
- matching domain: feasible caps, `GroupedSolverBisection`
  (strict/scalable/best_effort), child graph transitions (OCO/bracket,
  идемпотентные); app: `SolveGroupedBatch`.
- matching infra: `ExecutionGroups` producer (Kafka) + Postgres repo
  (идемпотентно, проверено против live PG) + active groups loader (live PG).
- Интеграция в `matching_loop` (gated на `MATCHING_POSTGRES_DSN`, аддитивно к
  single-leg F-04).
- **filled_cum convergence (GAP-1 закрыт):** partial-группы накапливают
  `filled_cum` идемпотентно; combo → filled при исчерпании binding-ноги
  (ratio-максимум); пустые batch'и не публикуются.
- **Feature flags + policy (002):** `ComboPolicy` (флаги `f09_*_enabled` +
  лимиты) — честный create-gate в order_flow (env `F09_*`).
- **ExecutionGroup enrichment (062):** proto += `user_id` + LegResult
  `instrument_symbol`/`side`; `combo_orders` += `user_id`/`account_id`.
- **Ledger grouped postings (060):** `ApplyExecutionGroup` over `execution.groups`
  consumer (идемпотентно по `execution_group_id`).
- **Полный E2E (090) проверен LIVE:** multileg combo → matching → `execution.groups`
  → ledger position владельца.
- **Risk grouped (040):** `GroupPreTradeCheck` domain (AC-F09-006 + notional/legs/
  external лимиты) + `PreTradeCheckGroup` gRPC + order_flow `RiskClient` wiring
  (заменил stub-approve, fail-closed). Verified live (accept + fail-closed proof).

**MVP-2 завершён.** Полная вертикаль: order_flow (policy+risk) → matching (solver)
→ execution.groups → ledger positions. Дальше — MVP-3+ (spread/factor QP,
OCO/bracket, external legs, frontend/observability).
**Известные нюансы (feature README):** `exec_price=0` для не котируемых символов;
Kafka `risk.alerts` publish для grouped reject — небольшой follow-up (сейчас лог).
