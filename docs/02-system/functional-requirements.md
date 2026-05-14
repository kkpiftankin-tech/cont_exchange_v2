---
id: DOC-SYSTEM-FR
phase: 02-system
status: draft
owner: core-team
source:
  - IN-001 §6 «Функциональные бизнес-требования»
related:
  - docs/02-system/features/feature-index.md
  - docs/02-system/non-functional-requirements.md
---

# Functional Business Requirements

Перечень функциональных требований к Continuous Exchange System (CES) с уровня бизнеса. Технические детали — в feature-документах и контрактах.

Идентификатор формата `FR-<AREA>-NNN` для связи с тестами и trace-таблицами.

## FR-ORDER. Заявки

### FR-ORDER-001. Поддержка типов заявок

CES должна поддерживать:

1. **Мгновенные сделки** — «купить/продать сейчас» с максимально высокой скоростью и опциональным контролем диапазона цен.
2. **Растянутые во времени** — `FlowOrder` с заданным окном `[t_start, t_end]` и профилем скорости (TWAP-подобный или произвольный).
3. **Портфельные** — корзина активов с весами; исполнение согласовано.
4. **Парные / спреды** — частный случай портфельной.

Acceptance: F-02 ([UC-F02-01](use-cases/UC-F02-01-create-flow-order/use-case.md)), F-09 ([UC-F09-01](use-cases/UC-F09-01-create-combo-order/use-case.md)).

### FR-ORDER-002. Параметры заявки

Клиент обязан задавать:

- `symbol` / список инструментов с весами;
- `side` (buy/sell);
- ценовой диапазон `[p_low, p_high]`;
- объёмы (`q_rate`, `q_max`);
- окно исполнения (`window_start`, `window_end`) и `time-in-force`.

Acceptance: F-02.

### FR-ORDER-003. Управление заявкой

Клиент может изменить (`amend`) активную заявку (`p_low`, `p_high`, `q_rate`, `q_max`, окно) или полностью отменить (`cancel`). Изменения проходят повторную pre-trade проверку.

Acceptance: F-03 ([UC-F03-01](use-cases/UC-F03-01-amend-cancel-order/use-case.md)).

## FR-MM. Market making

### FR-MM-001. Публикация и обновление кривых

Market Maker может публиковать двустороннюю CSLO-кривую с параметрами `P_L`, `P_H`, `Q`, `U`, slope и менять её в реальном времени.

Acceptance: F-10 ([UC-F10-01](use-cases/UC-F10-01-publish-mm-curve/use-case.md)).

### FR-MM-002. Контроль инвентори

MM может задать caps по инвентори и видит inventory exposure, PnL и adverse selection в реальном времени.

Acceptance: F-10.

## FR-CLEAR. Клиринг

### FR-CLEAR-001. Непрерывный клиринг

Через интервал, заданный `solver_config.batch_interval_ms`, система:

- агрегирует все активные кривые/заявки,
- решает задачу равновесия (clear price + executed rates) по каждому инструменту,
- формирует `FillEvent` по каждой задействованной заявке,
- сохраняет `BatchResult` с диагностикой (residual norm, solve time, num_active_orders).

Acceptance: F-04 ([UC-F04-01](use-cases/UC-F04-01-run-batch-clearing/use-case.md)).

### FR-CLEAR-002. Детерминированность

При том же входе (та же история, та же `solver_config.version`) результаты клиринга идентичны для целей replay/audit.

Acceptance: F-15 ([UC-F15-01](use-cases/UC-F15-01-replay-historical-batch/use-case.md)).

## FR-MD. Market data

### FR-MD-001. Внутренние данные

Система публикует в реальном времени: clearing price, executed rate, depth, spread, recent fills.

Acceptance: F-05 ([UC-F05-01](use-cases/UC-F05-01-stream-market-data/use-case.md)).

### FR-MD-002. Внешние данные

CES интегрируется с CEX/DEX/AMM, нормализует их LOB в FOB-форму и использует как reference price + потенциально как источник ликвидности.

Acceptance: F-11 ([UC-F11-01](use-cases/UC-F11-01-ingest-external-marketdata/use-case.md)).

## FR-RISK. Управление риском

### FR-RISK-001. Pre-trade контроль

Каждая новая или изменённая заявка проходит проверку по лимитам `risk_limits` (notional, max position, max leverage, order rate, asset whitelist, kill_switch).

Решение: `ACCEPT` / `REJECT` / `THROTTLE` (с корректировкой `q_rate`/`q_max`) / `HALT`.

Acceptance: F-07 ([UC-F07-01](use-cases/UC-F07-01-pretrade-risk-check/use-case.md)).

### FR-RISK-002. Post-trade мониторинг

После каждого `BatchResult` Risk Manager пересчитывает позиции, margin coverage, exposure, и при необходимости генерирует:

- `risk.alerts(MARGIN_CALL)`,
- ликвидационный ордер.

Acceptance: F-08 ([UC-F08-01](use-cases/UC-F08-01-liquidate-position/use-case.md)).

### FR-RISK-003. Kill-switch

Operator может остановить торги глобально или по инструменту через operator console; событие фиксируется в `risk.alerts` и обрабатывается всеми затрагиваемыми сервисами.

Acceptance: F-16 ([UC-F16-01](use-cases/UC-F16-01-trigger-kill-switch/use-case.md)).

## FR-LEDGER. Учёт средств

### FR-LEDGER-001. Балансы и резервы

Ledger ведёт по каждой паре (user, asset) поля `free_balance`, `reserved_balance`, `venue_allocated`, `pending_transfer`. Резерв создаётся при активации заявки, освобождается при отмене или fill.

Acceptance: F-02, F-03, F-06 ([UC-F06-01](use-cases/UC-F06-01-show-positions/use-case.md)).

### FR-LEDGER-002. Применение fills

При получении `batch.outputs` Ledger атомарно обновляет балансы и позиции; идемпотентность по `batch_id + order_id`.

Acceptance: F-04, F-06.

### FR-LEDGER-003. Депозиты и выводы

Депозиты и выводы проходят через Blockchain / Custody Adapter, фиксируются в `collateral_transfers` со статусами `pending → processing → confirmed`.

Acceptance: F-14 ([UC-F14-01](use-cases/UC-F14-01-deposit-funds/use-case.md)).

## FR-HEDGE. Execution hedge

### FR-HEDGE-001. Формирование намерения

При достижении внутреннего inventory threshold система формирует `ExecutionIntent` (venue, symbol, side, qty, urgency).

Acceptance: F-12 ([UC-F12-01](use-cases/UC-F12-01-execute-hedge/use-case.md)).

### FR-HEDGE-002. Размещение и учёт

External Venues Connector размещает child orders на CEX/DEX, получает `ExecutionReport`, и Ledger корректирует `venue_allocated` балансы.

Acceptance: F-12.

## FR-REPORT. Отчётность

### FR-REPORT-001. Post-trade отчёт

Клиент может запросить отчёт по завершённой заявке: VWAP, IS (среднее, std, квантили), профиль исполнения во времени, декомпозиция IS на spread + temporary impact + permanent impact + volatility.

Acceptance: F-13 ([UC-F13-01](use-cases/UC-F13-01-generate-posttrade-report/use-case.md)).

### FR-REPORT-002. Регуляторная выгрузка

Compliance/Operator могут сформировать выгрузку по сделкам, позициям, kill-switch event'ам, ликвидациям в фиксированном формате.

Acceptance: F-13, F-17.

## FR-OPS. Операторская панель

### FR-OPS-001. Мониторинг

Operator видит состояние всех сервисов, текущие SLA метрики (solve time, residual norm, fill rate, latency), активные алерты.

Acceptance: F-16, F-17.

### FR-OPS-002. Управление параметрами

Operator может менять `risk_limits`, `solver_config`, fee model в рантайме без перезапуска; все изменения логируются для audit.

Acceptance: F-16.

## FR-AUTH. Аутентификация и доступ

### FR-AUTH-001. Регистрация и логин

Поддержка email/password, расширяемо до SSO/OAuth. После регистрации присваивается роль `demo` по умолчанию.

Acceptance: F-01 ([UC-F01-01](use-cases/UC-F01-01-authenticate-user/use-case.md)).

### FR-AUTH-002. Роли

Поддерживаемые роли: `demo`, `client`, `provider`, `operator`, `admin`. Operator может повышать роли при подтверждении KYC.

Acceptance: F-01.

## Связанные документы

- Каталог фич: [features/feature-index.md](features/feature-index.md)
- Acceptance per feature: блок «Acceptance Criteria» в каждом `features/F-XX-*/README.md`
- NFR: [non-functional-requirements.md](non-functional-requirements.md)
- Источник: IN-001 §6

## Source Fragments

- IN-001-FR-016
