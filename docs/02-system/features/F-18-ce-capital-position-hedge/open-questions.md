# Открытые вопросы — F-18 (v2) CE Virtual Counterparties (Band Hedge)

Источники: [`incoming-docs/2026-09-15-CE_algorithm_v2.md`](../../../../incoming-docs/2026-09-15-CE_algorithm_v2.md) §«Незакрытое» +
сводка ревью 12 агентов [F-18 v2 review summary](../../../implementation-plan/F-18-v2-agents-review-summary.md) §11/§13/§14/§15.

> Формат: `OQ-F18v2-NN` · **статус** (`open` / `blocking`) · **адресат** (кому решать).
> `blocking` = входит в единый гейт владельца (review §13); реализация Э1 не стартует до снятия.

---

## Доменные (из спеки «Незакрытое»)

### OQ-F18v2-01 — признание прибыли по факту расчёта

- **Вопрос.** Прибыль признаётся на **факте расчёта**, а не по клирингу (`realized += fq·(марка−px_факт)/1000·sgn`).
  В прототипе накапливается разрыв **до 6611 USD / 1200 тактов** между «прибылью по клирингу» и «прибылью
  по факту». Нужна явная политика: где фиксируется разрыв, куда относится (house?), допустимый порог.
- **Влияет на:** AC-F18v2-12 (A8 confirm), «Счёт дома», `INV-9`, CHK-38.
- **Статус:** `open`.
- **Адресат:** trading-domain-specialist + владелец (экономика house).

### OQ-F18v2-02 — задержка перевоза (3 такта)

- **Вопрос.** В прототипе `transfer_eta` = **3 такта** взята как константа. Обоснование величины,
  конфигурируемость per-route (Binance↔Kraken vs прочие), поведение при просрочке `eta` — не зафиксированы.
- **Влияет на:** AC-F18v2-17 (перевоз), `ce_transfer.eta`, `f05a_clearing_config.transfer_eta`, Э5.
- **Статус:** `open`.
- **Адресат:** data-schema-designer + trading-domain-specialist.

### OQ-F18v2-03 — выбор q при V>2

- **Вопрос.** При `V>2` площадках полос **столько же, сколько агентов** (не две константы `q_translator`/
  `q_arbitrageur`). Нужна схема назначения `q_j` per-agent (формула от глубины/риска? матрица конфигов?).
  Текущая модель (18/30) откалибрована только для 2×2.
- **Влияет на:** AC-F18v2-09 (полосы per-kind), масштабирование `f05a_clearing_config`.
- **Статус:** `open`.
- **Адресат:** trading-domain-specialist + владелец (конфиг-политика).

### OQ-F18v2-04 — цены при пустом клиринге

- **Вопрос.** При пустом клиринге (`|S|≤C` по всем кругам, поток нулевой) множители Лагранжа `π`
  недоопределены сверх gauge-фиксации `π[USD@Binance]=0`. Нужна **явная политика** цен для пустого такта
  (нести предыдущие `π`? брать mid? помечать такт как «без цен»?).
- **Влияет на:** AC-F18v2-05 (A4/A5), альтернативный поток A-1 (пустой круг), CHK-08 (`DeadZoneNoFlow`).
- **Статус:** `open`.
- **Адресат:** trading-domain-specialist + matching (движок).

---

## Архитектурные (из review §13 — блокирующий гейт)

### OQ-F18v2-05 — консолидация движков клиринга (ADR-062)

- **Вопрос.** Три пути клиринга не сведены: `ce_agent_clearing` (наш), `vector_qp_solver`/OSQP (F-05A),
  `ContinuousClearingSolver` (Engine A/B). Нужен отдельный ADR (кандидат **ADR-062**) — какой движок
  канонический для такта агентов, как переиспользуется `ClearCe` / `vector_qp_pi`.
- **Влияет на:** AC-F18v2-05 (closed-form vs OSQP), размещение кода matching, T-F18-101..103.
- **Статус:** `blocking`.
- **Адресат:** solution-architect + владелец.

### OQ-F18v2-06 — party_type AGENT как архитектурное решение (ADR-063)

- **Вопрос.** AGENT — **третий** тип участника (не подтип CLIENT): дилерский инвентарь, знаковый, без
  резерва/неотрицательности. `CLIENT`/`HOUSE` — generated column в `accounts`; `AGENT` — отдельная таблица
  `ce_agent_position` (несовместимые инварианты). Требуется **ADR-063** для фиксации.
- **Влияет на:** AC-F18v2-16 (party_type AGENT), схема `accounts`/`ce_agent_position`, ledger layering.
- **Статус:** `blocking`.
- **Адресат:** solution-architect + data-schema-designer + владелец.

---

## Безопасность (review §11 — 2 CRITICAL)

### OQ-F18v2-07 — CRITICAL-1: нет auth-middleware на /v1/flow-orders

- **Вопрос.** `POST/GET/DELETE /v1/flow-orders` (`http_gateway.cpp:882-1010`) — **нет** auth-middleware,
  `user_id` берётся из тела запроса. Любой может действовать «за» произвольный `user_id`, включая будущий
  `__ce_house__` / `agent_id`. Строковый split CLIENT/AGENT/HOUSE в ledger защищён только строкой id →
  обходится с этого эндпоинта. До Э1: **починить** (gateway auth) **либо** явно принять как отслеживаемый
  риск с компенсирующим контролем.
- **Влияет на:** AC-F18v2-16 (изоляция AGENT/HOUSE), целостность позиций агентов.
- **Статус:** `blocking`.
- **Адресат:** security-reviewer + владелец.

### OQ-F18v2-08 — CRITICAL-2: PreHedgeCheck не в живом пути, kill-switch не останавливает

- **Вопрос.** `risk.PreHedgeCheck` **не подключён** к живому пути отправки (`cpp/venues/src/` не вызывает
  risk). `SetKillSwitch(halt=true)` не останавливает уже эмитированные заявки. Полоса/эмиссия v2 (A7)
  унаследует этот обход, как сегодняшний F-12. До Э1: включить PreHedgeCheck в путь venues **либо** принять
  риск явно.
- **Влияет на:** AC-F18v2-08/10 (A7 эмиссия), реюз F-12 PreHedgeCheck, безопасность kill-switch.
- **Статус:** `blocking`.
- **Адресат:** security-reviewer + владелец.

---

## Тестовые входы (review §15)

### OQ-F18v2-09 — отсутствуют golden-источники ce_tick_v2.py / square_bands.py

- **Вопрос.** `ce_tick_v2.py` (27 проверок) и `square_bands.py` (39 проверок) **физически отсутствуют**
  в репозитории (в `incoming-docs/`, `docs/10-testing/reference/ce-agents/` и где-либо ещё). Без них
  golden-числа U14 / CHK-40..66 и допуски 1e-6/1e-9 некалибруемы — используется только грубый sanity-check.
  Нужны **от владельца**; регистрация — отдельным вызовом skill `ingest-docs` (не частью T-F18-012).
- **Влияет на:** AC-F18v2-18 (эффект полосы), CHK-40..66, весь `agent_band_test.cpp` числовой блок.
- **Статус:** `blocking` (тестовый гейт).
- **Адресат:** владелец (передать файлы) → docs-ingestion.

---

## Сводка

| OQ | Тема | Статус | Адресат | ADR/тикет |
| --- | --- | --- | --- | --- |
| OQ-F18v2-01 | признание прибыли по факту | open | domain + владелец | — |
| OQ-F18v2-02 | задержка перевоза 3 такта | open | data-schema + domain | Э5 |
| OQ-F18v2-03 | выбор q при V>2 | open | domain + владелец | конфиг |
| OQ-F18v2-04 | цены при пустом клиринге | open | domain + matching | — |
| OQ-F18v2-05 | консолидация движков | blocking | solution-architect | ADR-062 |
| OQ-F18v2-06 | party_type AGENT | blocking | solution-architect + data | ADR-063 |
| OQ-F18v2-07 | CRITICAL-1 gateway auth | blocking | security | — |
| OQ-F18v2-08 | CRITICAL-2 PreHedgeCheck/kill-switch | blocking | security | — |
| OQ-F18v2-09 | golden ce_tick_v2/square_bands | blocking | владелец | T-F18-012 |

> Зонтичный гейт над всеми `blocking`: **ADR-061** переводится в `accepted` только после решений §2 плана
> и снятия OQ-05..09 (review §13). До этого реализация Э1 закрыта (`CLAUDE.md §0a`, no-code-before-docs).
