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

- **Этап 7 (ADR-086/IN-019, 2026-10-27): «Гейты» сужен владельцем — без
  `level_gate.py`.** Сделано: CI-гейт `.github/workflows/consistency-gate.yml`
  (feature-yaml-checker, in013-checker, link-audit.py, consistency/check.py,
  свежесть `specs/registry/registry.yaml`), спроектирован и написан
  `devops-engineer` по обязательному триггеру `agent-routing.md` §2;
  `scripts/hooks/post_edit_hint.py` расширен (`capability_hints()`) —
  правка файла из `feature.yaml.capabilities[].codePaths` или
  `ui-map.yaml` component теперь называет CAP/UC/CALC/UI в
  `additionalContext` (не блокирует); попутно исправлен устаревший
  placeholder `capability: CAP-F18-NN` в `docs/frontend/ui-map.yaml` у
  `UI-VC-BAND-MODE`→`CAP-F18-BAND-WIDTH` и `UI-VC-SYSTEM-PNL`→
  `CAP-F18-SYSTEM-PNL` (комментарий был написан до Этапа 4, когда
  `capabilities[]` ещё не существовало — стал фактически неверным, не
  просто неполным). `UI-VC-POS-BASIS` остался с placeholder'ом по сути
  (ни одна из 3 заведённых capabilities[] F-18 не ссылается на него в
  своём `ui[]` — реальный пробел каталога возможностей, не баг разметки).
  НЕ сделано этим шагом (решение владельца 2026-10-27, не инженерная
  недоработка):
  - **`.claude/hooks/level_gate.py`** (блокирующий Stop-хук: правка
    `codePaths`/`ui-map` без CR или трейлера `Change: S|spike` → блок) —
    отложен целиком. За всю историю репозитория нет ни одного CR-файла и
    ни одного `Change:`-трейлера в коммите; включение хука сейчас
    останавливало бы любую будущую правку кода/docs, затрагивающую
    `codePaths`, с первого же коммита после включения.
  - **`buf breaking`** — `buf` не установлен ни на dev-хосте, ни в CI;
    отдельная задача.
  - **«Раннер примеров CALC»** — инструмента нет вообще, нужно строить с
    нуля (парсинг численного примера из CALC-карточки + исполнение против
    кода/формулы); не входит в «уже готово».
  - **Рекомендация брифа п.4** (короткие ветки по CR от `main`/от ветки
    F-18, не один PR №48 на всю ветку) — передана владельцу как
    рекомендация, не реализация; пока вся ветка F-18 идёт одним PR, новый
    CI-гейт видит только суммарный дифф этого PR, не дифф отдельного CR.

- **Этап 7: пред-существующий долг на `main`, из-за которого п.1–3
  `consistency-gate.yml` НЕ строго блокирующие (только `continue-on-error`,
  как и п.4)** — найдено `devops-engineer`, перепроверено независимо
  (git worktree на `main`, Python 3.12 — под CI-версией 3.11 находок у
  `in013-checker` больше, чем при первой оценке агента на локальном
  сломанном python3.9):
  - `feature-yaml-checker`: 3 ERROR — `F-05` два `codePaths` отсутствуют
    на диске (`cpp/market_data/src/infra/kafka_fills_consumer.cpp`,
    `clickhouse_marketdata_storage.cpp`), `F-09` status mismatch
    (`feature.yaml='implemented'` vs `feature-component-map='not-implemented'`).
  - `in013-checker`: H1/H6 нарушения (`;`/`:` в тексте сообщений Mermaid)
    минимум в 9 файлах `docs/02-system/.../sequences/` и
    `docs/05-components/.../sequences/`, включая `SEQ-LEDGER-001`,
    `SEQ-LEDGER-002`, `SEQ-RISK-001`, `SEQ-F05A-UC-F05A-02/04-services`,
    `SEQ-F20-UC-F20-01/02`.
  - `link-audit.py`: 28 broken links (часть — вероятные плейсхолдеры вида
    `[text](url)` в примерах README, тул их не отличает от настоящих
    битых ссылок).
  Ничего из этого не внесено Этапом 7 — все три найдены уже существующими
  на `main` до этого коммита. Закрывать вне мандата `devops-engineer`
  (правки `docs/02-system/`, `docs/05-components/`, markdown-ссылок — не
  его разрешённые пути); решение владельца 2026-10-27: не чинить сейчас,
  держать `continue-on-error` до отдельного захода. Снимать
  `continue-on-error` по каждому чекеру отдельно после закрытия его
  конкретного долга, не всем скопом разом.

- **Этап 8 (ADR-086/IN-019, 2026-10-27): «Пилот F-18» сужен владельцем до
  1 полного примера CR** — `docs/changes/CR-F18-001-arbitrageur-basis-capability.md`
  (первый реальный CR-файл в репозитории, формат §4.8). Закрыл разрыв
  п.2 брифа (первый названный пример: ADR-080 → CALC-CE-ARBITRAGEUR-BASIS):
  новая `CAP-F18-ARBITRAGEUR-BASIS` в `feature.yaml`, новый `AC-F18v2-29`
  (текст проверен `trading-domain-specialist` — учтена оговорка про флаг
  `CE_ARB_MARKET_HEDGE` и реверс ADR-083, которую я сам не учёл в
  черновике), `examples:` блок в CALC-карточке (число `0.246106`/tolerance
  `0.000001` — независимо пересчитано `trading-domain-specialist` и мной,
  не округлённое `0.246` из прозы карточки), `UC-F18-04.capabilities[]`
  заведён (ранее отсутствовал), `ui-map.yaml#UI-VC-POS-BASIS` получил
  реальную capability взамен placeholder `CAP-F18-NN` (закрывает находку
  Этапа 7). `tools/consistency/check.py` получил рабочий `--scope F-XX`
  (раньше был зарезервированным, не используется — Этап 2/4) — C-01/02/03/
  04/05/06/14/15/17 фильтруются по фиче, C-08/C-16 и ADR-ветки C-14/C-15
  остаются глобальными (table/waiver/ADR-узлы не несут поле `feature`,
  задокументировано в докстрингах правил).
  НЕ сделано этим шагом (решение владельца 2026-10-27, не инженерная
  недоработка):
  - **Остальные 21 ADR группы ADR-064…085** — ретроспективные CR `refine`
    не написаны; только ADR-080 (+сопутствующие ADR-073/069/083) получил
    разбор.
  - **Экраны `/ce-graph-live` и `/ce-treasury-live`** — не заведены в
    `ui-map.yaml` вообще (пилот Этапа 3 остаётся 1 экран из 3 нужных для
    полного анализа п.1); анализ-до-Р2 для их элементов не проводился.
  - **`docs/05-components/sequences/SEQ-F18-UC-F18-04-services.md`** —
    не обновлена; путь панели «Живые позиции» (BFF `buildAgentRows` →
    фронт) не покрыт текущей диаграммой такта клиринга.
  - **9 планов F-18 → 1 действующий** (`docs/implementation-plan/F-18-*`:
    `ce-band-hedge.status`, `ce-capital-position-hedge.tasks`,
    `dynamic-band-gamma`, `liquidity-band`, `maker-resting-limit`,
    `remediation`, `v2-agents-review-summary`, `v2.tasks`,
    `venues-exec-throughput`) — не тронуты, не сведены, не закрыты
    ссылками на CR.
  - **`feature.yaml.uxScreens` указывает несуществующий `/ce-agents-live`**
    (реальные роуты — `/ce-graph-live` и `/ce-treasury-live`, см.
    `frontend/web/src/App.js`) — замечено по брифу §4.1, не исправлено
    (вне объёма CR-F18-001, не связано с `CAP-F18-ARBITRAGEUR-BASIS`).
  - **`tools/calc-examples/` (раннер исполняемых примеров CALC, IN-019
    §4.5)** — не существует; `examples:` блок CR-F18-001 проверен вручную
    (см. CR, раздел «Проверка»), не автоматически.
  - **`level-drift --scope F-18`** — инструмента `level-drift` нет вообще
    (решение владельца не строить, см. запись Этап 7); приёмка Этапа 8 по
    этому пункту невычислима тем же образом, что и остальные непостроенные
    части приёмки Этапов 5-7.
  - Приёмка Этапа 8 §8 (`consistency/check.py --scope F-18` без ошибок)
    **не достигнута**: 11 error / 12 warn после CR-F18-001 (было 11/8) —
    рост WARN ожидаем (4 новых C-03 "ADR не ссылается обратно", тот же
    паттерн, что у 3 существующих capabilities Этапа 4), 11 ERROR —
    pre-existing находки Этапов 2/4 по F-18, не входящие в объём этого CR.
  - Побочная находка (не чинил, вне объёма): `tools/link-audit.py` не
    исключает `.claude/worktrees/**` из сканирования — локально (не в CI,
    там чистый checkout) раздувает счётчик "broken links" в 3× на этом
    дев-хосте из-за двух сторонних git worktree внутри дерева репозитория.

- **Этап 9 (ADR-086/IN-019, 2026-10-27): «Распространение и метрики»
  сужен владельцем до одной метрики** — «остальные фичи через describe/
  refine, без сплошной переписки» (п.1 брифа) — норма на будущее, не
  задача на реализацию, строить нечего. Из еженедельного списка метрик
  (п.2) построено только `tools/change_adoption_report.py` (доля
  коммитов с кодом, у которых есть CR-ссылка или `Change:`-трейлер,
  `--days N` как у `agents_report.py`; добавлен в `CLAUDE.md` §4 рядом с
  `agents_report.py`). Честный первый прогон: **0,0% за всю историю и за
  последние 7 дней** (349/45 коммитов с кодом соответственно) — ожидаемо,
  CR-механизм и трейлеры введены этим же днём (Этап 8, `CR-F18-001`), ни
  один более ранний коммит не мог на них ссылаться; мои собственные
  коммиты Этапов 1–8 не считаются "коммитами с кодом" по определению
  скрипта (меняют только `docs/`/`specs/`/`tools/`), поэтому они не
  искажают метрику вверх искусственно.
  НЕ сделано этим шагом (решение владельца 2026-10-27, не инженерная
  недоработка):
  - **`agents_report.py`, `tools/route/eval.py`** — из списка п.2 уже
    существуют (Этапы, предшествующие IN-019, и Этап 5 соответственно),
    повторно не строились.
  - **`level-drift`** — инструмента нет вообще (решение не строить,
    Этапы 6–8); метрика «доля с устаревшими нормативными уровнями»
    невычислима тем же образом.
  - **DORA-метрики целиком** (change lead time, частота выкладок на dev,
    change fail rate, deployment rework rate, MTTR) — **принципиально
    невычислимы сейчас**: нигде в репозитории нет журнала событий деплоя
    (успех/отказ/время). Навык `rebuild-service` пишет только временный
    лог на dev-хосте (`/tmp/*.log`, перезаписывается, не агрегируется,
    не персистентный). Предпосылка для DORA — сначала завести
    `.claude/logs/deploys.jsonl` (или аналог) с событием на каждый вызов
    `rebuild-service`/деплой; это отдельная инженерная задача, не часть
    этого шага (владелец явно решил не заводить её сейчас).

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
