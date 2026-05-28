---
id: ADR-009
status: accepted
date: 2026-05-20
owners:
  - core-team
related:
  - docs/02-system/features/F-15-backtest-replay/
  - cpp/backtest/src/infra/in_memory_shadow_ledger.cpp
  - incoming-docs/IN-006.fragment-map.md
---

# ADR-009: Shadow-mode isolation strategy for F-15 Replay

> **Status:** accepted (отражает текущую реализацию).
> **Источники:** IN-006 (F-15 spec), фактический код `cpp/backtest`.

## Контекст

F-15 Backtest / Replay требует, чтобы replay-сессии переиграли
исторические батчи через ту же бизнес-логику (Matching → Risk → Ledger),
что и production, **не задевая** production-балансы клиентов и
production-позиции.

Это означает, что для каждой сессии нужен изолированный экземпляр (или
эквивалент) Collateral Ledger. Возможные варианты:

1. **Namespace prefix в едином in-memory ledger.** Все балансы/резервы
   per session живут под ключом `shadow:<session_id>:<account_id>` в
   единой in-memory структуре, реализованной в
   `cpp/backtest/src/infra/in_memory_shadow_ledger.cpp`. Reuses
   `cpp/ledger/src/app/ledger_uc.cpp` (тот же домен-уровень).
2. **Separate PostgreSQL schema per session.** Каждая сессия получает
   schema `replay_<session_id>` с таблицами `accounts`, `positions`,
   `reservations`.
3. **Отдельный production-like Ledger process.** Полный поднимаемый
   ledger-сервис per session.

## Решение

Принят **вариант 1 (namespace prefix in-memory)** для текущего MVP.

## Обоснование

- **Скорость.** In-memory access на порядки быстрее PG INSERT/UPDATE при
  тысячах батчей в одной сессии. Это критично для SLO 1k батчей < 60 сек
  (IN-006-NFR-1).
- **Простота развёртывания.** Сессия живёт в рамках одного процесса
  `cpp/backtest`. Не нужны DDL миграции per session, нет orphan schemas.
- **Изоляция гарантируется на уровне ключа** — таблица под `shadow:`
  префиксом физически отделена от production namespace; production
  Collateral Ledger живёт в отдельном процессе.
- **Reuse доменного слоя.** `ledger_uc.cpp` используется без изменений
  (через `cmake` source dependency), что обеспечивает parity бизнес-логики.

## Trade-offs / последствия

- **Нет персистентности shadow state между перезапусками.** При crash
  backtest-service нужно перезапустить replay с нуля (через retry).
  Mitigation: RestoreState UC реконструирует промежуточное состояние из
  `replay_agentlogs` при дублирующем `original_batch_id`.
- **Memory footprint per session.** Большие портфели с десятками тысяч
  активных позиций увеличат memory. Mitigation: lazy initialization
  балансов; для load-теста 20 параллельных сессий рекомендован сервис с
  ≥ 8 GiB RAM (см. AC-N5).
- **Нет SQL-аналитики shadow-состояния.** Если потребуется внешний
  drill-down, нужно будет либо периодически снимать snapshot в `metrics`
  JSONB поле `replay_agentlogs`, либо мигрировать на вариант 2.

## Альтернативы и причины отказа

- **Separate PG schema.** Отклонён: сложность миграций, медленнее,
  orphan-schema cleanup усложнён.
- **Отдельный ledger process.** Отклонён: overkill для backtest scope,
  усложнение деплоя, performance penalty.

## Обратимость

Reversible. Если изоляция через namespace окажется недостаточной (например,
для multi-tenant compliance), можно мигрировать на вариант 2 (PG schema):

1. Добавить `replay_<session_id>` schema на старте сессии.
2. Реализовать `PostgresShadowLedger` через тот же `IShadowLedger` port.
3. Удалить schema при retention expiry.

Без изменений в use cases или domain layer.

## Связанные артефакты

- Код: [cpp/backtest/src/infra/in_memory_shadow_ledger.cpp](../../../cpp/backtest/src/infra/in_memory_shadow_ledger.cpp), [cpp/backtest/src/app/shadow_namespace_uc.cpp](../../../cpp/backtest/src/app/shadow_namespace_uc.cpp)
- Тест: [cpp/backtest/tests/shadow_ledger_apply_test.cpp](../../../cpp/backtest/tests/shadow_ledger_apply_test.cpp), [cpp/backtest/tests/shadow_namespace_test.cpp](../../../cpp/backtest/tests/shadow_namespace_test.cpp)
- Doc: [docs/05-components/backtest-service/overview.md §Shadow Isolation](../../05-components/backtest-service/overview.md#shadow-isolation), [docs/04-domain/business-rules.md §ShadowPositions](../../04-domain/business-rules.md#shadowpositions-per-session-in-memory)
- AC: AC-N6 + IT-2 в [F-15 acceptance-criteria](../../02-system/features/F-15-backtest-replay/acceptance-criteria.md#2-нефункциональные-требования)
- Open question: [F-15 open-questions §2](../../02-system/features/F-15-backtest-replay/open-questions.md#2-replay-isolation-namespace-vs-schema)

## Source

- IN-006 § Компоненты (Collateral Ledger shadow mode)
- IN-006-FR-007 (F15-7)
