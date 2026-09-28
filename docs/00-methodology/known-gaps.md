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

При любой задаче явно указывай, является ли изменение:

- `docs-only`;
- `contract change`;
- `MVP implementation`;
- `production hardening`;
- `research/simulation`;
- `migration from legacy_mvp`.

Это высокоуровневый список; конкретный статус по фиче — `feature.yaml`
(`status: planned|stub|implemented`), не здесь.
