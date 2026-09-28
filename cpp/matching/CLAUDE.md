# cpp/matching — правила сервиса (перенесено из корневого CLAUDE.md §15)

Текущий `matching` — simulator. Целевой `matching` должен развиваться в
FOB Core. Общие правила слоёв/стиля C++ — `.claude/rules/cpp.md`; контракт
`BatchResult` — `.claude/rules/contracts.md`.

При изменении matching:

1. не ломай contract `BatchResult` без ADR;
2. сохраняй deterministic mode для replay;
3. отделяй solver algorithm от Kafka runtime;
4. добавляй diagnostics;
5. проверяй conservation constraints;
6. учитывай price interval каждой заявки;
7. учитывай speed caps;
8. учитывай risk caps, если они передаются в batch input;
9. сохраняй traceability: order → batch → fill → ledger → risk snapshot.

Минимальные тестовые сценарии для solver — `.claude/rules/testing.md`.

Полная модель непрерывного рынка (N-агентная агрегация, curve forms,
IN-012) — `docs/04-domain/business-rules.md`, раздел
`## Clearing Mechanics (IN-012)`; концептуальные термины (CSLO, BatchResult)
— `docs/04-domain/core-concepts-and-invariants.md`.

Триггер эскалации (`.claude/rules/agent-routing.md`): формулы/инварианты
клиринга → `trading-domain-specialist`; деньги (settlement) → `code-reviewer`.
