# cpp/venues — правила сервиса (перенесено из корневого CLAUDE.md §18)

`venues` сейчас частично симулятор, частично real REST-адаптер (`multi_real`
— см. memory `venues-execution-model`). Целевой External Venues Connector
должен:

- получать market data snapshots;
- нормализовать venue symbols;
- учитывать fees, tick size, lot size;
- принимать `ExecutionIntent`;
- отправлять child orders;
- возвращать `ExecutionReport`;
- корректно обрабатывать partial fill, reject, cancel, timeout;
- не принимать самостоятельных trading decisions без policy/intent.

Никогда не добавляй реальные credentials в репозиторий (`permissions.deny` в
`.claude/settings.json` блокирует чтение `infra/.env`; venue API keys той же
природы — не коммитить, только env).

Симулятор (`ROUTING_MODE_SHADOW` / sim REST-recent-trades matching, ADR-060)
и реальный REST-путь живут в одном сервисе — при правке сначала проверь,
какой режим активен, иначе легко "почините" не тот код путь.
