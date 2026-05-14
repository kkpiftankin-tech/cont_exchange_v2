---
id: DOC-SYSTEM-ACTORS
phase: 02-system
status: draft
owner: core-team
source:
  - IN-001 §4 «Роли и пользователи»
  - IN-001 §«Внешние участники»
related:
  - docs/01-business/stakeholders.md
  - docs/03-architecture/context-diagram.md
---

# Actors

Перечень внешних и внутренних участников, взаимодействующих с **Continuous Exchange System** (CES).

Подразделение:

- **Primary actors** — инициируют сценарии и являются основными потребителями ценности.
- **Supporting actors** — внешние системы, без которых сценарий невозможен, но не инициируют его.
- **Internal actors** — операторские роли внутри платформы.

## Primary actors

### Liquidity Trader

- Подаёт `FlowOrder` (одно-/портфельный), задавая `p_low`, `p_high`, `q_rate`, `q_max`, окно исполнения.
- Не имеет частной информации; цель — минимизировать **Implementation Shortfall** под целевой риск.
- Видит preview VWAP/IS, фактические fills, post-trade отчёт.

### Market Maker (LP)

- Публикует двусторонние CSLO-кривые «цена–скорость» с параметрами `P_L`, `P_H`, `Q`, `U`.
- Цель — стабильный PnL на спреде минус impact и adverse selection.
- Управляет инвентори в реальном времени.

### Client Integrator / Provider / Broker

- Подключается через API/FIX, реализует свои execution-алгоритмы поверх CES.
- Маршрутизирует заявки клиентов в `orders.normalized`.
- Получает агрегированные отчёты по своим клиентам.

## Internal actors

### Operator / Admin

- Конфигурирует matching, risk-policy, comissions, batch-interval, epsilon-liquidity.
- Активирует / деактивирует kill-switch (глобально или по инструменту).
- Получает алерты, расследует инциденты.

### Compliance Officer / Risk Officer

- Получает регуляторные выгрузки.
- Просматривает audit log сделок и решений.
- Управляет KYC-статусами и whitelist'ами.

### Researcher / Quant

- Запускает backtest/replay, оценивает альтернативные solver-конфиги и risk-политики.
- Анализирует `agent_logs`.

## Supporting actors (внешние системы)

### CEX (Centralized Exchange)

- Источник внешней ликвидности и referrence prices (Binance, Coinbase, Bybit, ...).
- Целевая площадка для **execution hedge**.

### DEX / AMM

- On-chain источник ликвидности (Uniswap-like xyk/stableswap).
- Используется как «pool of last resort» при недостатке внутренней ликвидности.

### Custody Provider

- Хранит активы клиентов и/или биржи.
- Подтверждает on-chain / банковские deposits и withdrawals.

### Blockchain

- Расчётный слой для on-chain deposits/withdrawals и (опционально) on-chain закрепления параметров mechanism.

### KYC / AML Provider

- Верификация личности, мониторинг подозрительной активности.
- Источник флагов для `users.kyc_status`.

### Regulator

- Получатель агрегированной и детализированной отчётности.
- Может потребовать реконструкцию истории сделок и решений.

### Analytics Platform

- Внешний потребитель анонимизированной статистики (spread, depth, fill rate, «воронка» KryptonÂ-типа).

### Backtest / Research Sandbox

- Отдельный environment, использующий те же бизнес-контракты, что и live, на исторических данных.

## Таблица матрица «actor × use case»

| Actor | Создание FlowOrder (UC-F02-01) | Amend/Cancel (UC-F03-01) | Batch clearing (UC-F04-01) | Stream MD (UC-F05-01) | Show positions (UC-F06-01) | Liquidation (UC-F08-01) | MM curve (UC-F10-01) | Execution hedge (UC-F12-01) | Post-trade report (UC-F13-01) | Deposit (UC-F14-01) | Replay (UC-F15-01) | Kill-switch (UC-F16-01) | Alert (UC-F17-01) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Liquidity Trader | ✅ | ✅ | (consumer) | ✅ | ✅ | (consumer) | — | (consumer) | ✅ | ✅ | — | (consumer) | — |
| Market Maker | (special) | ✅ | (consumer) | ✅ | ✅ | (consumer) | ✅ | (consumer) | ✅ | ✅ | — | (consumer) | — |
| Client Integrator | ✅ | ✅ | — | ✅ | ✅ | — | ✅ | — | ✅ | ✅ | — | — | — |
| Operator | — | — | (monitors) | (monitors) | (monitors) | (manual override) | — | (monitors) | (provides) | (approves) | — | ✅ | (handles) |
| Compliance | — | — | — | — | — | — | — | — | ✅ | — | — | — | — |
| Researcher | — | — | — | — | — | — | — | — | — | — | ✅ | — | — |
| CEX | — | — | — | (data source) | — | — | — | (target) | — | — | (historical) | — | — |
| DEX | — | — | — | (data source) | — | — | — | (target) | — | — | — | — | — |
| Custody | — | — | — | — | — | — | — | — | — | ✅ | — | — | — |
| KYC/AML | (registration prereq) | — | — | — | — | — | — | — | — | (check) | — | — | — |
| Regulator | — | — | — | — | — | — | — | — | (recipient) | — | — | (audit) | (recipient) |

## Related

- Stakeholders (business roles): [../01-business/stakeholders.md](../01-business/stakeholders.md)
- Context diagram (actors as external systems): [../03-architecture/context-diagram.md](../03-architecture/context-diagram.md)
- Features per actor: [features/feature-index.md](features/feature-index.md)

## Source Fragments

- IN-001-FR-006 — §6 Участники и роли источника
- IN-001-FR-014 — §4 Типы пользователей источника
- IN-001-FR-021 — внешние участники
