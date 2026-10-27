# Отчёт: критичная функциональность и вопросы для эксперта

Сгенерировано `tools/expert_review_report.py`. Источник истины — файлы,
на которые ведут ссылки ниже; этот отчёт — только навигация по текущему
состоянию, не редактируется руками. Перегенерировать после любой правки
`open-questions.md`, статуса CALC-карточки, ADR или `known-gaps.md`.

## Сводка

- **Blocking** вопросов (нельзя продолжать фичу): **5**
- Открытых структурированных вопросов (`OQ-*`): **17**
- Фич со старым списком вопросов без структуры (мигрировать по касанию): **4** (97 пунктов суммарно)
- Карточек вычислений не в статусе `implemented`: **9**
- ADR в статусе `proposed` (ждут решения владельца): **13**
- Инженерных долгов в known-gaps.md: **2**

## Blocking — нельзя продолжать фичу, пока не снято

- **OQ-F18v2-05** (F-18) — консолидация движков клиринга (ADR-062) · адресат: solution-architect + владелец · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-06** (F-18) — party_type AGENT как архитектурное решение (ADR-063) · адресат: solution-architect + data-schema-designer + владелец · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-07** (F-18) — CRITICAL-1: нет auth-middleware на /v1/flow-orders · адресат: security-reviewer + владелец · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-08** (F-18) — CRITICAL-2: PreHedgeCheck не в живом пути, kill-switch не останавливает · адресат: security-reviewer + владелец · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-09** (F-18) — отсутствуют golden-источники ce_tick_v2.py / square_bands.py · адресат: владелец (передать файлы) → docs-ingestion · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

## Открытые вопросы для эксперта (по адресату)

### data-schema-designer + trading-domain-specialist

- **OQ-F18v2-02** (F-18) — задержка перевоза (3 такта) · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### trading-domain-specialist

- **OQ-F18v2-17** (F-18) — переводчик тоже реагирует на фантомный сигнал, а не только арбитражёр · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### trading-domain-specialist + matching (движок)

- **OQ-F18v2-04** (F-18) — цены при пустом клиринге · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### trading-domain-specialist + владелец (конфиг-политика)

- **OQ-F18v2-03** (F-18) — выбор q при V>2 · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### trading-domain-specialist + владелец (экономика house)

- **OQ-F18v2-01** (F-18) — признание прибыли по факту расчёта · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### владелец

- **OQ-F18v2-13** (F-18) — задержка рыночных данных относительно хеджа (2026-10-16) · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-14** (F-18) — Coinbase: перенос торговли с пар к USDT на пары к USD/USDC · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-15** (F-18) — способ измерения доступной ликвидности (окно уровней книги vs по объёму) · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-18** (F-18) — включать ли в продакшене скидку на возраст котировки и предел по капиталу переводчика · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-19** (F-18) — возвращать ли арбитражёра к модели «только перевозка, без выхода на рынок» · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-20** (F-18) — источник дохода арбитражёра по продукту · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-21** (F-18) — делать ли честный симулятор исполнения режимом по умолчанию · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### владелец + solution-architect

- **OQ-F18v2-12** (F-18) — верификация коэффициента конвертации $\kappa$ связки и калибровка новых пределов глубины · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

### владелец + trading-domain-specialist

- **OQ-F18v2-16** (F-18) — баланс «ловить реальные возможности» против «не давать ложных сигналов» · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-22** (F-18) — физический перевод остаётся единственным путём вперёд для прибыльности арбитражёра · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-23** (F-18) — допустимая погрешность цены для порогов свежести данных · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)
- **OQ-F18v2-24** (F-18) — завышенная оценка волатильности из объединённого потока бирж · [docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md](docs/02-system/features/F-18-ce-capital-position-hedge/open-questions.md)

## Старые списки вопросов без OQ-ID/статуса (нужна структуризация)

Формат — `docs/00-methodology/artifact-templates.md` "Open Questions Template"; мигрировать по касанию фичи, не разом.

- **F-04** — 9 вопрос(ов) без структуры · [docs/02-system/features/F-04-batch-clearing/open-questions.md](docs/02-system/features/F-04-batch-clearing/open-questions.md)
- **F-11** — 25 вопрос(ов) без структуры · [docs/02-system/features/F-11-external-venues-lob-to-fob/open-questions.md](docs/02-system/features/F-11-external-venues-lob-to-fob/open-questions.md)
- **F-12** — 15 вопрос(ов) без структуры · [docs/02-system/features/F-12-execution-hedge/open-questions.md](docs/02-system/features/F-12-execution-hedge/open-questions.md)
- **F-15** — 48 вопрос(ов) без структуры · [docs/02-system/features/F-15-backtest-replay/open-questions.md](docs/02-system/features/F-15-backtest-replay/open-questions.md)

## Карточки вычислений не в статусе `implemented`

- **CALC-CE-ANCHOR** (F-18, `partial`) — Общий якорь дилера a* и порог хеджа Q_i (эталонная модель IN-017) · [docs/04-domain/calculations/CALC-CE-ANCHOR.md](docs/04-domain/calculations/CALC-CE-ANCHOR.md)
- **CALC-CE-CLEARING-SURPLUS** (F-18, `implemented (BFF, 2026-10-22)`) — Выгода участника в клиринге CE (теоретический излишек S = w²/2α) · [docs/04-domain/calculations/CALC-CE-CLEARING-SURPLUS.md](docs/04-domain/calculations/CALC-CE-CLEARING-SURPLUS.md)
- **CALC-CE-LINK-TRANSFER** (F-18, `implemented (ledger, dev; 2026-10-22)`) — Физический перевод связки CE между биржами (порог, сумма, штуки, остаток паритета) · [docs/04-domain/calculations/CALC-CE-LINK-TRANSFER.md](docs/04-domain/calculations/CALC-CE-LINK-TRANSFER.md)
- **CALC-CE-SKEW-BAND-CALIBRATION** (F-18, `calibrated`) — Калибровка порога (band) и функции сдвига цены (skew) под максимум заработка переводчика · [docs/04-domain/calculations/CALC-CE-SKEW-BAND-CALIBRATION.md](docs/04-domain/calculations/CALC-CE-SKEW-BAND-CALIBRATION.md)
- **CALC-CE-SYSTEM-PNL** (F-18, `partially implemented (торговый результат и переоценка запасов — BFF, 2026-10-22; инварианты §4 — логи ledger)`) — P&L системы CE (MTM реальных остатков) и инварианты · [docs/04-domain/calculations/CALC-CE-SYSTEM-PNL.md](docs/04-domain/calculations/CALC-CE-SYSTEM-PNL.md)
- **CALC-CE-TRANSLATOR-DEPTH** (F-18, `implemented (за флагами, по умолчанию выключено; 2026-10-26)`) — Итоговая глубина переводчика CE — вес по возрасту, множитель исполнения и капитальный предел · [docs/04-domain/calculations/CALC-CE-TRANSLATOR-DEPTH.md](docs/04-domain/calculations/CALC-CE-TRANSLATOR-DEPTH.md)
- **CALC-CE-TRANSLATOR-PNL** (F-18, `proposed`) — Прибыль переводчика — realized (закрытые round-trip) и unrealized (MTM остатка) · [docs/04-domain/calculations/CALC-CE-TRANSLATOR-PNL.md](docs/04-domain/calculations/CALC-CE-TRANSLATOR-PNL.md)
- **CALC-F12-HEDGE-PNL** (F-12, `drift`) — Hedge PnL (результат внешнего хеджа) · [docs/04-domain/calculations/CALC-F12-HEDGE-PNL.md](docs/04-domain/calculations/CALC-F12-HEDGE-PNL.md)
- **CALC-F18-VENUE-LIQUIDITY-STATS** (F-18, `implemented (шаг 1, множитель глубины, и шаг 2, бюджет ленты на заявки; оба выключены флагами CE_DEPTH_FILL_COEF и CE_ORDER_CAP_TAPE)`) — Ликвидность внешней биржи — оборот ленты ν и коэффициент исполнения f · [docs/04-domain/calculations/CALC-VENUE-LIQUIDITY-STATS.md](docs/04-domain/calculations/CALC-VENUE-LIQUIDITY-STATS.md)

## ADR в статусе `proposed` (ждут решения владельца)

- **ADR-010** (—) — ADR-010-agentlog-replay-agentlog-unification · [docs/03-architecture/adr/ADR-010-agentlog-replay-agentlog-unification.md](docs/03-architecture/adr/ADR-010-agentlog-replay-agentlog-unification.md)
- **ADR-024** (—) — ADR-024-latency-aware-venue-curve · [docs/03-architecture/adr/ADR-024-latency-aware-venue-curve.md](docs/03-architecture/adr/ADR-024-latency-aware-venue-curve.md)
- **ADR-044** (F-06) — Модель балансов ledger — единый PG-источник vs dual in-memory + PG · [docs/03-architecture/adr/ADR-044-ledger-balance-source-of-truth.md](docs/03-architecture/adr/ADR-044-ledger-balance-source-of-truth.md)
- **ADR-045** (F-06) — Модель соединений с PostgreSQL — per-call connection vs in-app pool · [docs/03-architecture/adr/ADR-045-pg-connection-pooling.md](docs/03-architecture/adr/ADR-045-pg-connection-pooling.md)
- **ADR-046** (F-06) — Топик positions.update для push-обновлений позиций (F6-5) · [docs/03-architecture/adr/ADR-046-positions-update-topic.md](docs/03-architecture/adr/ADR-046-positions-update-topic.md)
- **ADR-070** (F-11) — Быстрая полоса стакана venues — свежий сырой стакан для панели «Торговля» · [docs/03-architecture/adr/ADR-070-venues-fast-depth-lane-for-raw-book-display.md](docs/03-architecture/adr/ADR-070-venues-fast-depth-lane-for-raw-book-display.md)
- **ADR-074** (F-18) — Честный симулятор исполнения — forward-only филл по биржевому времени · [docs/03-architecture/adr/ADR-074-honest-execution-simulator.md](docs/03-architecture/adr/ADR-074-honest-execution-simulator.md)
- **ADR-075** (F-18) — Синхронные рыночные данные — биржевое время снимка и age-aware якорь · [docs/03-architecture/adr/ADR-075-synchronous-market-data-anchors.md](docs/03-architecture/adr/ADR-075-synchronous-market-data-anchors.md)
- **ADR-076** (F-18) — Топология графа переводов CE — полная сетка бирж против звезды вокруг дома · [docs/03-architecture/adr/ADR-076-ce-transfer-graph-topology.md](docs/03-architecture/adr/ADR-076-ce-transfer-graph-topology.md)
- **ADR-077** (F-05A) — Окно глубины стакана для кривой ликвидности — по объёму сделки, а не по числу уровней · [docs/03-architecture/adr/ADR-077-liquidity-depth-window-by-volume.md](docs/03-architecture/adr/ADR-077-liquidity-depth-window-by-volume.md)
- **ADR-079** (F-18) — Диагностика решения CE-клиринга — снимок последнего такта из matching, показ на вкладке «Граф» с объяснением «почему потока нет» · [docs/03-architecture/adr/ADR-079-ce-clearing-decision-diagnostics.md](docs/03-architecture/adr/ADR-079-ce-clearing-decision-diagnostics.md)
- **ADR-081** (F-18) — Ширина полосы хеджа CE — ручная установка и автоматический режим по волатильности · [docs/03-architecture/adr/ADR-081-ce-band-width-manual-and-volatility-modes.md](docs/03-architecture/adr/ADR-081-ce-band-width-manual-and-volatility-modes.md)
- **ADR-082** (F-18) — Измерение ликвидности внешних бирж и её обратная связь в глубину агента, размер заявки и порог хеджа CE · [docs/03-architecture/adr/ADR-082-venue-liquidity-measurement-and-feedback.md](docs/03-architecture/adr/ADR-082-venue-liquidity-measurement-and-feedback.md)

## Инженерные долги (docs/00-methodology/known-gaps.md)

- Auth + audit на operator-эндпоинтах `f05a_clearing_config`
- `matching_loop.cpp` poison-message backstop без метрики и без теста
