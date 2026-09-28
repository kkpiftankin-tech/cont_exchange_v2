# cpp/risk — правила сервиса (перенесено из корневого CLAUDE.md §16)

Risk Manager должен сохранять следующие принципы:

- pre-trade checks до активации заявки;
- проверка notional, max position, leverage, order rate, whitelist;
- `RiskDecision` может быть ACCEPT, REJECT, RESIZE, HALT;
- kill-switch может быть global или instrument-specific;
- risk alerts должны быть observable и replayable;
- post-trade risk должен иметь связь с `batch_id`.

Любое изменение risk policy требует:

1. обновления `docs/02-system/non-functional-requirements.md` или
   `docs/02-system/functional-requirements.md`;
2. обновления `docs/04-domain/business-rules.md`
   (`docs/04-domain/core-concepts-and-invariants.md` для базовых терминов);
3. обновления `docs/07-data/risk-schema.md` или соответствующего файла
   (см. также `.claude/rules/data.md`);
4. тестов на false positive/false negative cases;
5. operator runbook, если меняется kill-switch/liquidation.

Триггер эскалации (`.claude/rules/agent-routing.md`): формулы/политики риска
→ `trading-domain-specialist`; auth/kill-switch/аудит → `security-reviewer`;
деньги → `code-reviewer`.
