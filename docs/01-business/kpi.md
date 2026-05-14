---
id: DOC-BUSINESS-KPI
phase: 01-business
status: draft
owner: core-team
---

# KPI

## Качество исполнения

- **Median IS** для крупных flow orders ≤ benchmark на CLOB.
- **p95 IS** в пределах заявленного доверительного интервала.
- **Fill rate** ≥ 99% для активных flow orders в окне исполнения.

## Производительность

- **median solveTimeMs** ≤ 500 ms.
- **p95 solveTimeMs** ≤ 1000 ms.
- **Uptime** ≥ 99.9% (MVP), ≥ 99.99% (prod).

## Качество рынка

- **Bid-ask spread** не хуже эталонного CLOB на топ-парах.
- **Depth** на уровнях ±0.5%, ±1% от mid.
- **Доля internal liquidity** в общем объёме fills.

## Бизнес

- **Активные клиенты** в месяц.
- **Объём торгов** в quote-валюте.
- **Доход от комиссий**.
- **NPS / реклама́ций**.

## Регуляторные

- **% KYC verified** среди торгующих клиентов.
- **Доля AML-flagged транзакций**.
- **Время отклика на регуляторный запрос**.
