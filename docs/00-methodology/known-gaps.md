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

- **`feature.yaml.status` и `specs/domain/feature-component-map.yaml[F-XX].status`
  используют разные схемы после Этап 1 (ADR-086/IN-019 §4.4, 2026-10-27).**
  `feature.yaml` перешёл на `lifecycle: planned|active|deprecated` +
  `status: {docs, code, runtime}`; `feature-component-map.yaml` остался на
  старой плоской строке (`in-progress-mvp`, `docs-complete-code-gated`, ...).
  `tools/feature-yaml-checker/check.py` больше не падает ERROR на этом
  расхождении (понижено до WARN — две разные схемы нельзя сравнивать строкой
  в строку), но сама рассинхронизация не устранена. Решение по полной миграции
  `feature-component-map.yaml` на новую схему (или по её выводу из
  эксплуатации как дубликата — сам файл дублирует feature.yaml, что отдельная
  давняя проблема) не принято; не делать молча в рамках точечных правок.

- **`F-18.relatedAdrs` переименован из `parentRequirements`** (Этап 1 п.6,
  ADR-086/IN-019, 2026-10-27) — это поле ошибочно хранило ADR, а не
  `FR-*` бизнес-требования (конвенция `relatedAdrs`/`architectureDecisions`
  уже использовалась в F-06/F-09/F-05A). F-18 и F-20 при этой правке
  остались с `parentRequirements: []` — у обеих фич вообще нет `FR-*` в
  `docs/02-system/functional-requirements.md` (в отличие от остальных 17
  фич + F-05A, где перенос был механическим по явной строке
  `Acceptance: F-XX`). Нужно ли писать retroactive `FR-CE-*`/`FR-SIM-*` —
  продуктовое решение, не инженерное; зафиксировано как
  `OQ-F18v2-25`/`OQ-F20-01`, не решено здесь.

- **Первый прогон `tools/consistency/check.py` (Этап 2, ADR-086/IN-019,
  2026-10-27) нашёл 17 пред­существующих находок** (не внесены этим
  прогоном, инструмент только обнаружил):
  - `C-04` (4): `UC-F12-04`, `UC-F12-05`, `UC-F15-05`, `UC-F15-06` —
    `status: active`, но нет `service-sequence` в frontmatter и нет файла
    L1-диаграммы вообще (`.claude/rules/sequences.md` требует её для
    активного UC).
  - `C-05` (2): `CALC-F05A-DEPTH-WINDOW.md` и `CALC-F18-VENUE-LIQUIDITY-STATS.md`
    (файл `CALC-VENUE-LIQUIDITY-STATS.md`) — элемент списка `code:` в
    frontmatter содержит неэкранированное `:` внутри комментария в скобках,
    YAML парсит его как вложенный маппинг, а не строку (сломанная шапка).
  - `C-08` (6): таблицы `execution_reports_raw`, `sim_hedge_pnl`,
    `replay_fee_models`, `replay_reward_configs`, `combo_compensations`,
    `f05a_clearing_config_history` существуют в `infra/postgres/init.sql`,
    но не упомянуты ни в одном файле `docs/07-data/`.
  - `C-15` (5): `id` в шапке CALC-карточки ≠ имени файла — `CALC-BOOK-VWAP.md`
    (id `CALC-F11-BOOK-VWAP`), `CALC-CE-ARBITRAGEUR-BASIS.md` (id
    `CALC-F18-CE-ARBITRAGEUR-BASIS`), `CALC-CE-CLEARING-DIAG.md` (id
    `CALC-F18-CE-CLEARING-DIAG`), `CALC-CE-GRAPH.md` (id `CALC-F18-CE-GRAPH`),
    `CALC-VENUE-LIQUIDITY-STATS.md` (id `CALC-F18-VENUE-LIQUIDITY-STATS`).
  Не исправлено этим шагом (построение инструмента ≠ исправление того, что
  он нашёл) — повторный `python3 tools/consistency/check.py` воспроизводит
  полный список в любой момент, см. `tools/consistency/README.md`.

- **Этап 4 п.1 (ADR-086/IN-019, 2026-10-27): пилот `capabilities[]` у F-18
  (3 из 28 `AC-F18v2-*`) включил `C-02/C-03/C-06/C-17` — 7 новых WARN,
  ожидаемых, не исправлены этим шагом:**
  - `C-03` (6): `ADR-081`, `ADR-066`, `ADR-083` (×2), `ADR-084` (×2) не
    содержат текста своего `CAP-F18-*` — ожидаемо, эти ADR написаны до
    появления `capabilities[]`; обратная ссылка — отдельная правка текста
    ADR, не делалась.
  - `C-17` (1): `CAP-F18-BAND-WIDTH` имеет `status.code: done` и
    `AC-F18v2-21`, но `tests: []` — совпадает с `CALC-CE-SKEW-BAND-CALIBRATION.tests`
    (тоже `[]`, калибровка значений, не код-путь), не новая находка по
    существу.
  Остальные 18 фич и 25 из 28 AC у F-18 не размечены `capabilities[]` —
  правила C-02/03/06/17 для них молча ничего не проверяют (пусто = нечего
  проверять, не ошибка), не полный каталог возможностей.

- **Этап 5 (ADR-086/IN-019, 2026-10-27): `tools/route/eval.py` на
  `golden.yaml` (20 кейсов) не проходит приёмку §8** (якорь ≥18/20,
  верхняя точка ≥16/20, класс ≥16/20) — честный первый результат: якорь
  11/20, верхняя точка 13/20, класс 14/20, тип (без порога) 17/20.
  Структурная причина — не баг: word-overlap скорер без LLM (<200мс по
  требованию брифа) не умеет различать «баг дисплея» vs «баг формулы»
  для типа `fix` и не умеет судить, полна ли цепочка документации для
  класса `ask`/`describe` — это требует шагов 2-6 алгоритма §3.5
  (runtime-first, горизонтальный переход, суждение), которые принципиально
  нужны реальному контексту, не тексту реплики. Подробный разбор —
  `tools/route/README.md`. `.claude/rules/agent-routing.md` НЕ удалён
  (решение владельца 2026-10-27) и остаётся действующим — `route.py`
  работает как источник кандидатов-якорей, не как замена маршрутизации.

- **Этап 6 (ADR-086/IN-019, 2026-10-27): «Агенты под уровни» сужен владельцем
  до безопасной уборки** — `skills:` preload (`04-proto-contract-designer`
  ← `register-kafka-topic`, `05-data-schema-designer` ← `register-pg-table`,
  подтверждено как реальный frontmatter-ключ Claude Code перед правкой),
  раздел «Правая ветвь» у `09-test-architect` (таблица Р→проверка из §3.2),
  аудит P-01 (13 из 16 агентов без `Edit`/`Write` и с `permissionMode: plan`
  — **уже верно**, не регресс и не правка), `13-docs-ingestion-engineer`
  уже ограничен `incoming-docs/` в собственном теле — не трогал. НЕ сделано
  этим шагом (решение владельца 2026-10-27, не инженерная недоработка):
  - Переписать `description` всех 16 агентов под формат §7 (Р-уровень,
    «Use when CR отмечает уровень X changed», Not for, Returns) — отложено,
    т.к. карточка CR с полем «уровень changed» нигде не производится (Этап 5
    сузил `tools/route/` до `--candidates`, не строил `change-routing.md`);
    переписывать `description` под несуществующий процесс — риск разойтись
    с `.claude/rules/agent-routing.md`, который остаётся действующим.
  - `env.CLAUDE_CODE_FORK_SUBAGENT: "0"` и `rebuild-service:
    disable-model-invocation: true` — отложено явным решением владельца:
    обе меняют поведение агента во всех будущих сессиях/для всех
    контрибьюторов, не только в рамках этой задачи.
  - Хук `InstructionsLoaded` → `.claude/logs/instructions.jsonl` — не
    реализован: такого типа хука в Claude Code нет (сверено со списком
    используемых: `SessionStart`/`UserPromptSubmit`/`PostToolUse`/
    `SubagentStart`/`SubagentStop`/`Stop`); потребовалась бы приблизительная
    эмуляция (например, `PreToolUse` на `Read` конкретных `CLAUDE.md`/
    `.claude/rules/*.md`), не буквальная реализация брифа.
  - **Приёмка Этапа 6 в принципе не может быть посчитана сейчас**:
    «агенты Р3–Р8 вызываются только в CR, где их уровень `changed`» требует
    поля «уровень changed» в `.claude/logs/agents.jsonl` за неделю — такого
    поля не производит ничто в репозитории (CR-карточка не строится, см.
    выше), посчитать нельзя не только за неделю, но и в любой момент без
    отдельной работы по генерации этого поля.

При любой задаче явно указывай, является ли изменение:

- `docs-only`;
- `contract change`;
- `MVP implementation`;
- `production hardening`;
- `research/simulation`;
- `migration from legacy_mvp`.

Это высокоуровневый список; конкретный статус по фиче — `feature.yaml`
(`lifecycle: planned|active|deprecated` + `status: {docs, code, runtime}`,
Этап 1 ADR-086/IN-019), не здесь.
