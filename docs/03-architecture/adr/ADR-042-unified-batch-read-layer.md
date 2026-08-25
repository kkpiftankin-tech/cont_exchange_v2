---
id: ADR-042
title: Единый read-слой батчей (single-leg + combo) через ClickHouse view
status: accepted
date: 2026-06-16
level: sea
feature: F-09
supersedes: []
related: [ADR-040, ADR-041]
---

# ADR-042 — Единый read-слой батчей (одномерные + многоногие combo)

## Контекст

Во вкладке «Батчи» (Профиль) отображались только **одномерные** батчи (F-04):

```
matching.run_batch → BatchResult → Kafka batch.outputs → market_data → ClickHouse batchresults (OLAP)
```

**Многоногие (combo)** батчи идут отдельным независимым циклом и НЕ попадают в
`batch.outputs`/ClickHouse:

```
matching.run_grouped_batch → ExecutionGroup → Kafka execution.groups + PostgreSQL execution_groups (OLTP)
```

Из-за этого combo-исполнение было невидимо в Профиле. Требование владельца:
«батчи одномерных и многоногих заявок должны храниться/отображаться вместе».

## Рассмотренные варианты

1. **BFF merge на чтении** — `/api/batches` читает ClickHouse + PostgreSQL и
   объединяет в коде. Быстро, но логика слияния в node-BFF, два запроса, и
   «вместе» только в ответе API.
2. **Дублировать combo в ClickHouse** (consumer `execution.groups` →
   `grouped_execution_events`). Проблема: `execution_groups` — **мутабельное
   OLTP-состояние** (group_status active→partial→filled, execution_scale растёт
   по циклам, ноги меняют статус). Дублирование мутабельных строк в append-OLAP
   требует ReplacingMergeTree + повторную эмиссию на каждое изменение → лаг и
   риск рассинхрона с authoritative-PG.
3. **Федерация PG→ClickHouse через PostgreSQL table engine + UNION view**
   (выбрано). Combo остаётся authoritative в PostgreSQL (его естественный дом для
   транзакционного состояния), а ClickHouse предоставляет **единый read-слой**
   `batch_results_unified`, объединяющий иммутабельные `batchresults` и живые
   combo-строки из PG.

## Решение

ClickHouse:

```sql
-- Федерация живых combo-групп из PostgreSQL (authoritative OLTP).
CREATE TABLE combo_groups_pg (...)
  ENGINE = PostgreSQL('postgres:5432','cex','execution_groups','cex','cex');

-- Единый read-слой: batchresults (kind='single_leg') ∪ combo_groups_pg (kind='combo_group').
CREATE VIEW batch_results_unified AS
  SELECT ..., 'single_leg'  AS kind, ... FROM batchresults
  UNION ALL
  SELECT ..., 'combo_group' AS kind, ... FROM combo_groups_pg;
```

- BFF `/api/batches` читает **только** `batch_results_unified` (одна выборка),
  отдаёт `kind` + combo-поля (`comboId`, `executionScale`, `ratioDeviationBps`).
- Профиль (`BatchDiagnosticsTable`) помечает combo-строки бейджем `combo`.
- `status` для single_leg считается из `residual_norm`, для combo — из
  `group_status` (filled→SUCCESS, failed/rejected/cancelled→FAILED, иначе PARTIAL).

Схема закреплена в `infra/clickhouse/init.sql`.

## Альтернативы (почему не выбраны)

- Вариант 1 — «вместе» только в API, логика слияния в BFF; не масштабируется на
  другие потребители (backtest, observability-запросы).
- Вариант 2 — дублирование мутабельного OLTP-состояния в OLAP; источник истины
  раздваивается, нужен механизм повторной эмиссии и дедупликации.

## Последствия

- ✅ Единый источник для «батчей» в ClickHouse; combo + single вместе, по времени.
- ✅ Нет дублирования источника истины: combo живёт только в PG, single — в CH.
- ✅ Нет изменения контракта `BatchResult` и кода matching/market_data.
- ⚠️ Каждый запрос view дёргает PostgreSQL (engine). Для текущих объёмов
  (десятки combo-групп) приемлемо; при росте — materialized-снимок или
  периодический ETL combo→CH.
- ⚠️ `/api/batches/{id}` (детализация) пока ищет в `batchresults`; для combo
  деталь — на странице Combo (`/api/combo-orders/{id}`). Унификация деталки —
  отдельный таск.
- ⚠️ ClickHouse зависит от доступности PostgreSQL для combo-части view (single_leg
  читается независимо).

## Обратимость

Высокая. Откат: вернуть `/api/batches` на `batchresults`, удалить view и
`combo_groups_pg`. Данные не мигрировались — combo по-прежнему в PG.

## Трассировка

- Feature: [F-09](../../02-system/features/)
- Data: [docs/07-data/batch-results-unified.md](../../07-data/batch-results-unified.md),
  `infra/clickhouse/init.sql` (`combo_groups_pg`, `batch_results_unified`)
- Code: `frontend/api/server.js` (`fetchBatchesFromClickHouse`),
  `frontend/web/src/pages/Profile/BatchDiagnosticsTable.js`
