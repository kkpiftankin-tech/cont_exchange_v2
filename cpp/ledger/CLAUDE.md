# cpp/ledger — правила сервиса (перенесено из корневого CLAUDE.md §17)

Ledger — источник истины по balances/positions.

Запрещено:

- применять fill дважды;
- освобождать резерв без idempotency key;
- смешивать user balance и exchange hedge balance;
- обновлять balances без audit trail;
- делать settlement на основе floating-point money (только
  `cex::common::Decimal` — см. `.claude/rules/data.md` и CLAUDE.md §3).

Для ledger flows всегда фиксируй:

- reservation id;
- user id;
- order id;
- batch id или execution report id;
- source topic / source service;
- before/after balance snapshot, если это продовый ledger path.

## Hedge PnL — известное место расхождения

Формула `calculate_hedge_pnl` (`src/app/ledger_uc.cpp`) — gross, без вычитания
комиссий; это НЕ баг сам по себе, но `docs/04-domain/business-rules.md`
исторически описывал net-версию. Прежде чем менять эту функцию — прочитай
[`docs/04-domain/calculations/CALC-F12-HEDGE-PNL.md`](../../docs/04-domain/calculations/CALC-F12-HEDGE-PNL.md)
целиком (путь числа, три места хранения/расчёта, открытые вопросы) и
обнови карточку вместе с кодом. Любая правка здесь — триггер `code-reviewer`
(money invariants) в `.claude/rules/agent-routing.md`.
