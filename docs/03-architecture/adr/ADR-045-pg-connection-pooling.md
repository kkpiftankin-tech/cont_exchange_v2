---
id: ADR-045
title: Модель соединений с PostgreSQL — per-call connection vs in-app pool
status: proposed
date: 2026-06-29
level: sea
feature: F-06
related: [ADR-004, ADR-044]
---

# ADR-045 — Модель соединений с PostgreSQL: per-call connection vs in-app pool

## Контекст

ledger и risk работают с PostgreSQL ([ADR-004](ADR-004-postgres-clickhouse-data-platform.md))
по схеме **«новое соединение на каждый вызов»**: на каждый gRPC-вызов
(`GetPositions`, `ApplyBatchResult`, `ReserveFunds`, `buildRiskSnapshot`, …) код
открывает новый `pqxx::connection`, выполняет транзакцию и закрывает соединение.

Под нагрузкой это упирается в `max_connections=100` PostgreSQL: установление
TCP + аутентификация + backend-fork на каждый вызов дорого, а параллельные вызовы
быстро исчерпывают лимит соединений. На замерах при ~500 одновременных запросах
наблюдается **~50% ошибок** (отказ установить соединение / превышение лимита).

F-06 усугубляет проблему: ApplyBatchResult и buildRiskSnapshot выполняются на
каждый батч по всем затронутым пользователям, а mirror-проводки резервов
([ADR-044](ADR-044-ledger-balance-source-of-truth.md)) добавляют PG-запись в горячий
путь reserve/release на каждое размещение ордера. Per-call connection становится
явным узким местом.

## Решение

Ввести **in-app connection pool** в каждом сервисе, работающем с PostgreSQL:

1. Общая реализация `cex::common::PgConnectionPool` (в `cpp/common`, т.к. это
   действительно cross-service утилита, CLAUDE.md §10.2) — фиксированный/ограниченный
   набор переиспользуемых `pqxx::connection`, выдача/возврат через RAII-handle,
   ожидание свободного соединения с таймаутом.
2. **Размер пула — через env** (`cex::common::Env`, без хардкода, CLAUDE.md §12.3),
   отдельно на сервис, с безопасным dev-default для docker-compose.
3. **pgbouncer — будущая опция** на уровне инфраструктуры (transaction pooling
   перед PostgreSQL), не исключающая in-app пул: суммарный размер всех in-app пулов
   должен укладываться в `max_connections` (или в лимит pgbouncer), что задаётся
   конфигурацией, а не кодом.

## Альтернативы

- **Single shared connection + mutex.** Простейший вариант без исчерпания лимита,
  но сериализует все PG-операции сервиса → не масштабируется, превращает БД-доступ
  в глобальный лок; неприемлемо для ApplyBatchResult/риск-снапшотов под нагрузкой.
- **pgbouncer-only (без in-app пула).** Снимает стоимость backend-fork, но
  per-call `pqxx::connection` всё равно платит за установление клиентского
  соединения и round-trip на каждый вызов; требует обязательного развёртывания
  pgbouncer как зависимости. Не решает проблему на уровне приложения и dev-стенда.
- **Поднять `max_connections` без пула.** Маскирует симптом: каждое PG-backend-
  соединение стоит памяти, тысячи короткоживущих соединений деградируют сервер;
  не устраняет стоимость открытия/закрытия на каждый вызов.

## Последствия

- ✅ Переиспользование соединений → снимается стоимость connect/auth/fork на вызов,
  исчезают отказы из-за исчерпания `max_connections` под целевой нагрузкой.
- ✅ Предсказуемая верхняя граница соединений на сервис (размер пула) → суммарную
  нагрузку на PostgreSQL можно планировать.
- ✅ Делает практичными mirror-проводки [ADR-044](ADR-044-ledger-balance-source-of-truth.md)
  и пер-батчевые риск-снапшоты в горячем пути.
- ⚠️ Управление жизненным циклом соединений: detect/recover «битых» соединений
  (reconnect), таймаут ожидания свободного, корректный graceful shutdown (CLAUDE.md §12.2).
- ⚠️ Нужна конфигурационная дисциплина: Σ(размеров пулов) ≤ `max_connections`
  (или лимит pgbouncer), иначе проблема исчерпания вернётся.
- ⚠️ Транзакционные границы должны оставаться внутри одного одолженного соединения
  (RAII-handle), без утечки соединения между транзакциями.

## Обратимость

Высокая. Откат: вернуть per-call `pqxx::connection`, отключив/убрав
`PgConnectionPool`; транзакционная семантика и SQL не меняются — меняется только
способ получения соединения. Схема БД не затрагивается.

## Трассировка

- Feature: [F-06](../../02-system/features/F-06-positions-pnl-margin/feature.yaml)
  (нагрузочные критерии — T-F06-062)
- Code (целевые точки): `cpp/common` (новый `PgConnectionPool`),
  `cpp/ledger/src/infra/postgres/*`, `cpp/risk/src/infra/*` (получение соединения).
- Config: env-переменная размера пула на сервис (`infra/env/.env-example`).
- Related: [ADR-004](ADR-004-postgres-clickhouse-data-platform.md) (PostgreSQL OLTP),
  [ADR-044](ADR-044-ledger-balance-source-of-truth.md) (PG в горячем пути reserve/release).
