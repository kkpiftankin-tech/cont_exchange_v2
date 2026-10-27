# Known gaps / planned evolution

Перенесено из корневого CLAUDE.md §26 (2026-09-28).

Текущий skeleton не является полной продукционной биржей. Ожидаемые зоны
развития:

- полноценный solver для FOB/CSLO вместо simulator;
- persistent PostgreSQL для OLTP;
- ClickHouse ingestion для истории и аналитики;
- полноценный Web UI / Trading Frontend;
- Auth & Identity / KYC integration;
- portfolio orders and multi-leg matching;
- market-maker curves;
- execution hedge policies;
- external venue adapters;
- backtest/replay engine;
- operator panel;
- production-grade observability;
- security hardening.

## Конкретные долги (tracked)

- **Auth + audit на operator-эндпоинтах `f05a_clearing_config`** (CLAUDE.md §1.7 —
  operator actions под авторизацией и audit trail). Группа BFF POST-эндпоинтов меняет
  клиринговые параметры без auth/аудита (кто/когда): `POST /api/clearing/slope-method`
  (способ наклона §6.2), `venue_stale_ms`, `batch_window_ms`, `ce_taker_fee_bps` и др.
  Пробел **pre-existing** для всей группы (не регресс конкретного изменения); закрывать
  всей группой разом (auth-guard + запись в audit-топик/таблицу), не поштучно. Выявлено
  code-review F-05A §6.2 (2026-10-01).

- **`matching_loop.cpp` poison-message backstop без метрики и без теста** (F-18/ADR-085,
  инцидент 2026-10-26). `consume_orders_loop` ловит
  `std::out_of_range` вокруг `on_ce_clearing_input` и логирует отказ, но не
  вызывает `metrics_.ObserveError(...)`, хотя все соседние failure-ветки в этом
  же файле это делают (:2132, :2145, :2163, :2291, :2365) — отказ виден только
  в логах, не в метриках/алертах. Нет теста, фиксирующего сам инвариант «только
  `std::out_of_range` долетает до этой точки» (`ce_graph_assembler_test.cpp`
  существует, этот кейс не покрывает). Не блокирует — defense-in-depth уже
  работает, корневая причина закрыта выше по потоку (`ce_input_guard.hpp` в
  `market_data`, AC-F18v2-28). Выявлено `code-reviewer` (money-path review,
  2026-10-27): добавить `metrics_.ObserveError("ce_clearing_input_rejected")` в
  catch-блок и `EXPECT_THROW(..., std::out_of_range)` в `ce_graph_assembler_test.cpp`.

При любой задаче явно указывай, является ли изменение:

- `docs-only`;
- `contract change`;
- `MVP implementation`;
- `production hardening`;
- `research/simulation`;
- `migration from legacy_mvp`.

Это высокоуровневый список; конкретный статус по фиче — `feature.yaml`
(`status: planned|stub|implemented`), не здесь.
