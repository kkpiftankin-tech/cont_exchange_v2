# Отчёт: критичная функциональность и вопросы для эксперта

Сгенерировано `tools/expert_review_report.py`. Источник истины — файлы,
на которые ведут ссылки ниже; этот отчёт — только навигация по текущему
состоянию, не редактируется руками. Перегенерировать после любой правки
`open-questions.md`, статуса CALC-карточки, ADR или `known-gaps.md`.

## Сводка

- **Blocking** вопросов (нельзя продолжать фичу): **5**
- Открытых структурированных вопросов (`OQ-*`): **27**
- Фич со старым списком вопросов без структуры (мигрировать по касанию): **4** (97 пунктов суммарно)
- Карточек вычислений не в статусе `implemented`: **7**
- ADR в статусе `proposed` (ждут решения владельца): **13**
- Инженерных долгов в known-gaps.md: **16**

## Blocking — нельзя продолжать фичу, пока не снято

- **OQ-F18v2-05** (F-18) — консолидация движков клиринга (ADR-062) · адресат: solution-architect + владелец · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-06** (F-18) — party_type AGENT как архитектурное решение (ADR-063) · адресат: solution-architect + data-schema-designer + владелец · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-07** (F-18) — CRITICAL-1: нет auth-middleware на /v1/flow-orders · адресат: security-reviewer + владелец · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-08** (F-18) — CRITICAL-2: PreHedgeCheck не в живом пути, kill-switch не останавливает · адресат: security-reviewer + владелец · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-09** (F-18) — отсутствуют golden-источники ce_tick_v2.py / square_bands.py · адресат: владелец (передать файлы) → docs-ingestion · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

## Открытые вопросы для эксперта (по адресату)

### data-schema-designer + trading-domain-specialist

- **OQ-F18v2-02** (F-18) — задержка перевоза (3 такта) · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### trading-domain-specialist

- **OQ-F18v2-17** (F-18) — переводчик тоже реагирует на фантомный сигнал, а не только арбитражёр · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-29** (F-18) — цена оценки связки и нумерарные связки · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-36** (F-18) — заявки LIMIT с немедленным исполнением при выключенном резерве мейкер-заявок · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### trading-domain-specialist + matching (движок)

- **OQ-F18v2-04** (F-18) — цены при пустом клиринге · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### trading-domain-specialist + владелец (конфиг-политика)

- **OQ-F18v2-03** (F-18) — выбор q при V>2 · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### trading-domain-specialist + владелец (экономика house)

- **OQ-F18v2-01** (F-18) — признание прибыли по факту расчёта · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### владелец

- **OQ-F18v2-13** (F-18) — задержка рыночных данных относительно хеджа (2026-10-16) · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-14** (F-18) — Coinbase: перенос торговли с пар к USDT на пары к USD/USDC · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-15** (F-18) — способ измерения доступной ликвидности (окно уровней книги vs по объёму) · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-18** (F-18) — включать ли в продакшене скидку на возраст котировки и предел по капиталу переводчика · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-19** (F-18) — возвращать ли арбитражёра к модели «только перевозка, без выхода на рынок» · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-20** (F-18) — источник дохода арбитражёра по продукту · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-21** (F-18) — делать ли честный симулятор исполнения режимом по умолчанию · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### владелец (решение продуктовое — писать ли FR-* постфактум)

- **OQ-F18v2-25** (F-18) — у F-18 нет FR-* бизнес-требования уровня functional-requirements.md · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F20-01** (F-20) — у F-20 нет FR-* бизнес-требования уровня functional-requirements.md · [docs/02-system/features/F-20-live-venue-simulator/open-questions.md](../02-system/features/F-20-live-venue-simulator/open-questions.md)

### владелец + solution-architect

- **OQ-F18v2-12** (F-18) — верификация коэффициента конвертации $\kappa$ связки и калибровка новых пределов глубины · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### владелец + trading-domain-specialist

- **OQ-F18v2-16** (F-18) — баланс «ловить реальные возможности» против «не давать ложных сигналов» · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-22** (F-18) — физический перевод остаётся единственным путём вперёд для прибыльности арбитражёра · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-23** (F-18) — допустимая погрешность цены для порогов свежести данных · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-24** (F-18) — завышенная оценка волатильности из объединённого потока бирж · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-28** (F-18) — связка: внутренний результат «на бумаге» и счёт дома · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-31** (F-18) — хедж ноги связки: закрывает ли он книгу или наращивает · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-32** (F-18) — R кросс-пар: фиксировать в USDT в момент сделки? · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-33** (F-18) — устаревание справедливой цены · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-34** (F-18) — комиссия на графике сделок: сторона, ставки по биржам, смысл taker-ставки · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-35** (F-18) — комиссия на графике: текущие ставки или зафиксированные на момент заявки · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](../02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

## Старые списки вопросов без OQ-ID/статуса (нужна структуризация)

Формат — `docs/00-methodology/artifact-templates.md` "Open Questions Template"; мигрировать по касанию фичи, не разом.

- **F-04** — 9 вопрос(ов) без структуры · [docs/02-system/features/F-04-batch-clearing/open-questions.md](../02-system/features/F-04-batch-clearing/open-questions.md)
- **F-11** — 25 вопрос(ов) без структуры · [docs/02-system/features/F-11-external-venues-lob-to-fob/open-questions.md](../02-system/features/F-11-external-venues-lob-to-fob/open-questions.md)
- **F-12** — 15 вопрос(ов) без структуры · [docs/02-system/features/F-12-execution-hedge/open-questions.md](../02-system/features/F-12-execution-hedge/open-questions.md)
- **F-15** — 48 вопрос(ов) без структуры · [docs/02-system/features/F-15-backtest-replay/open-questions.md](../02-system/features/F-15-backtest-replay/open-questions.md)

## Карточки вычислений не в статусе `implemented`

- **CALC-CE-AGENT-PNL** (F-18, `accepted`) — Заработок агента CE по биржам — R, U и R+U внутренней биржи, внешних бирж и итого · [docs/04-domain/calculations/CALC-CE-AGENT-PNL.md](../04-domain/calculations/CALC-CE-AGENT-PNL.md)
- **CALC-CE-ANCHOR** (F-18, `partial`) — Общий якорь дилера a* и порог хеджа Q_i (эталонная модель IN-017) · [docs/04-domain/calculations/CALC-CE-ANCHOR.md](../04-domain/calculations/CALC-CE-ANCHOR.md)
- **CALC-CE-PAIR-TRADES** (F-18, `accepted`) — Сделки на внешних биржах по паре — накопленный объём базовой валюты, цены покупки и продажи и цены с расчётной комиссией · [docs/04-domain/calculations/CALC-CE-PAIR-TRADES.md](../04-domain/calculations/CALC-CE-PAIR-TRADES.md)
- **CALC-CE-SKEW-BAND-CALIBRATION** (F-18, `calibrated`) — Калибровка порога (band) и функции сдвига цены (skew) под максимум заработка переводчика · [docs/04-domain/calculations/CALC-CE-SKEW-BAND-CALIBRATION.md](../04-domain/calculations/CALC-CE-SKEW-BAND-CALIBRATION.md)
- **CALC-CE-SYSTEM-PNL** (F-18, `partial`) — P&L системы CE (MTM реальных остатков) и инварианты · [docs/04-domain/calculations/CALC-CE-SYSTEM-PNL.md](../04-domain/calculations/CALC-CE-SYSTEM-PNL.md)
- **CALC-CE-TRANSLATOR-PNL** (F-18, `partial`) — Прибыль переводчика — realized (закрытые round-trip) и unrealized (MTM остатка) · [docs/04-domain/calculations/CALC-CE-TRANSLATOR-PNL.md](../04-domain/calculations/CALC-CE-TRANSLATOR-PNL.md)
- **CALC-F12-HEDGE-PNL** (F-12, `drift`) — Hedge PnL (результат внешнего хеджа) · [docs/04-domain/calculations/CALC-F12-HEDGE-PNL.md](../04-domain/calculations/CALC-F12-HEDGE-PNL.md)

## ADR в статусе `proposed` (ждут решения владельца)

- **ADR-010** (—) — ADR-010-agentlog-replay-agentlog-unification · [docs/03-architecture/adr/ADR-010-agentlog-replay-agentlog-unification.md](../03-architecture/adr/ADR-010-agentlog-replay-agentlog-unification.md)
- **ADR-024** (—) — ADR-024-latency-aware-venue-curve · [docs/03-architecture/adr/ADR-024-latency-aware-venue-curve.md](../03-architecture/adr/ADR-024-latency-aware-venue-curve.md)
- **ADR-044** (F-06) — Модель балансов ledger — единый PG-источник vs dual in-memory + PG · [docs/03-architecture/adr/ADR-044-ledger-balance-source-of-truth.md](../03-architecture/adr/ADR-044-ledger-balance-source-of-truth.md)
- **ADR-045** (F-06) — Модель соединений с PostgreSQL — per-call connection vs in-app pool · [docs/03-architecture/adr/ADR-045-pg-connection-pooling.md](../03-architecture/adr/ADR-045-pg-connection-pooling.md)
- **ADR-046** (F-06) — Топик positions.update для push-обновлений позиций (F6-5) · [docs/03-architecture/adr/ADR-046-positions-update-topic.md](../03-architecture/adr/ADR-046-positions-update-topic.md)
- **ADR-070** (F-11) — Быстрая полоса стакана venues — свежий сырой стакан для панели «Торговля» · [docs/03-architecture/adr/ADR-070-venues-fast-depth-lane-for-raw-book-display.md](../03-architecture/adr/ADR-070-venues-fast-depth-lane-for-raw-book-display.md)
- **ADR-074** (F-18) — Честный симулятор исполнения — forward-only филл по биржевому времени · [docs/03-architecture/adr/ADR-074-honest-execution-simulator.md](../03-architecture/adr/ADR-074-honest-execution-simulator.md)
- **ADR-075** (F-18) — Синхронные рыночные данные — биржевое время снимка и age-aware якорь · [docs/03-architecture/adr/ADR-075-synchronous-market-data-anchors.md](../03-architecture/adr/ADR-075-synchronous-market-data-anchors.md)
- **ADR-076** (F-18) — Топология графа переводов CE — полная сетка бирж против звезды вокруг дома · [docs/03-architecture/adr/ADR-076-ce-transfer-graph-topology.md](../03-architecture/adr/ADR-076-ce-transfer-graph-topology.md)
- **ADR-077** (F-05A) — Окно глубины стакана для кривой ликвидности — по объёму сделки, а не по числу уровней · [docs/03-architecture/adr/ADR-077-liquidity-depth-window-by-volume.md](../03-architecture/adr/ADR-077-liquidity-depth-window-by-volume.md)
- **ADR-079** (F-18) — Диагностика решения CE-клиринга — снимок последнего такта из matching, показ на вкладке «Граф» с объяснением «почему потока нет» · [docs/03-architecture/adr/ADR-079-ce-clearing-decision-diagnostics.md](../03-architecture/adr/ADR-079-ce-clearing-decision-diagnostics.md)
- **ADR-081** (F-18) — Ширина полосы хеджа CE — ручная установка и автоматический режим по волатильности · [docs/03-architecture/adr/ADR-081-ce-band-width-manual-and-volatility-modes.md](../03-architecture/adr/ADR-081-ce-band-width-manual-and-volatility-modes.md)
- **ADR-082** (F-18) — Измерение ликвидности внешних бирж и её обратная связь в глубину агента, размер заявки и порог хеджа CE · [docs/03-architecture/adr/ADR-082-venue-liquidity-measurement-and-feedback.md](../03-architecture/adr/ADR-082-venue-liquidity-measurement-and-feedback.md)

## Инженерные долги (docs/00-methodology/known-gaps.md)

- Auth + audit на operator-эндпоинтах `f05a_clearing_config`
- `matching_loop.cpp` poison-message backstop без метрики и без теста
- `feature.yaml.status` и `specs/domain/feature-component-map.yaml[F-XX].status` используют разные схемы после Этап 1 (ADR-086/IN-019 §4.4, 2026-10-27).
- `F-18.relatedAdrs` переименован из `parentRequirements`
- Первый прогон `tools/consistency/check.py` (Этап 2, ADR-086/IN-019, 2026-10-27) нашёл 17 пред­существующих находок
- Этап 4 п.1 (ADR-086/IN-019, 2026-10-27): пилот `capabilities[]` у F-18 (3 из 28 `AC-F18v2-*`) включил `C-02/C-03/C-06/C-17` — 7 новых WARN, ожидаемых, не исправлены этим шагом:
- Этап 5 (ADR-086/IN-019, 2026-10-27): `tools/route/eval.py` на `golden.yaml` (20 кейсов) не проходит приёмку §8
- Этап 6 (ADR-086/IN-019, 2026-10-27): «Агенты под уровни» сужен владельцем до безопасной уборки
- Этап 7 (ADR-086/IN-019, 2026-10-27): «Гейты» сужен владельцем — без `level_gate.py`.
- Этап 7: пред-существующий долг на `main`, из-за которого п.1–3 `consistency-gate.yml` НЕ строго блокирующие (только `continue-on-error`, как и п.4)
- Этап 8 (ADR-086/IN-019, 2026-10-27): «Пилот F-18» сужен владельцем до 1 полного примера CR
- Этап 9 (ADR-086/IN-019, 2026-10-27): «Распространение и метрики» сужен владельцем до одной метрики
- Учёт заработка агентов (ADR-087): известные ограничения
- R и U едины, остальные цифры панели «Живые позиции» — нет
- График «Заработок во времени» (CR-F18-005, 2026-10-28): известные ограничения.
- Расчётная комиссия на графике сделок по парам (CR-F18-007, 2026-10-28): известные ограничения.
