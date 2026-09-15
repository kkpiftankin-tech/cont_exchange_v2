---
id: DOC-F18-V2-REVIEW-SUMMARY
phase: implementation-plan
status: review-consolidation
owner: core-team
source:
  - incoming-docs/2026-09-15-CE_algorithm_v2.md
  - incoming-docs/2026-09-15-CE_integration_plan.md
  - docs/implementation-plan/CE-v2-agents-rework.plan.md
level: kite
---

# F-18 v2 — сводка прогона через всех агентов Claude (документация + код)

**Обновление 2026-09-15.** Гейт §13 частично снят: ADR-061/062/063 переведены в `accepted` (решение владельца), развилки §2 (движок = Newton, AGENT = третий party_type, фича = F-18) подтверждены. Документарная цепочка открыта. Остаются `blocking` для стадий кода: два security-CRITICAL (§11) и golden-источники `ce_tick_v2.py`/`square_bands.py` (§15). Разделы §13–§15 ниже сохранены как исходная карта ревью.

> **Что это.** Материал CE v2 (`incoming-docs/2026-09-15-CE_algorithm_v2.md`) прогнан через **12 специалист-агентов** проекта. Ниже — что произвёл каждый агент (видимый результат), межагентные пересечения и корректировки, единый блокирующий гейт владельца. **Кода эта сводка НЕ создаёт**: по `CLAUDE.md §0a` (no-code-before-docs) реализация Э1 не открывается, пока владелец не подтвердит решения §2 плана и ADR-061 не переведён в `accepted`. Это карта результатов, а не команда на реализацию.

## 0. Как читать

| Ось | Агенты | Артефакт-выход |
|---|---|---|
| **Документация** | business-analyst, system-analyst, solution-architect, proto-contract-designer, data-schema-designer, frontend-architect, test-architect, implementation-planner | BR/AC, feature.yaml/UC/SEQ, ADR, proto, DDL, UI-спеки, test-plan, T-F18-NNN |
| **Код (ревизия того, что есть)** | trading-domain-specialist, cpp-service-architect, code-reviewer, security-reviewer | инварианты, layering, баги, security-гейты |

Порядок ниже — от бизнеса к коду.

---

## 1. business-analyst — бизнес-требования

- **6 бизнес-требований** BR-F18v2-01..06: агенты = только переводчики+арбитражёры; позиция с нуля/знаковая; интернализация через полосу `±q`; прибыль на счёт дома; отказ от target; `Z_a = владение(t) − владение(0)`.
- **Метрика ценности:** полоса `q=18/30` даёт **−63% издержек хеджа** (182,73 vs 495,06 USD на 30 тактов), итог `+203,18` против `−246,03`. **Оговорка:** без встречного клиентского потока полоса ухудшает результат (гасить нечего) — это механизм интернализации, не универсальный выигрыш.
- **AGENT vs CLIENT/HOUSE:** AGENT — дилерский инвентарь, знаковый, без резерва; ≠ клиент (у клиента позиция ≥0 и reserve/release). Рекомендация: **третий тип участника**, не подтип клиента.

## 2. system-analyst — системный уровень

- **feature.yaml F-18 v2**, use cases **UC-F18-04..07**, L0/L1 Mermaid sequences, **18 acceptance criteria** AC-F18v2, FR/NFR без target-семантики.
- L1: `market_data → matching → risk → ledger → venues` + Kafka `ce.position.delta` / `execution.intents` / (Э5) `ce.transfers`, с contract+data binding таблицами.

## 3. solution-architect — архитектура/ADR

- **ADR-061** (новый) — supersede ADR-057, revises ADR-055/056/058.
- **ADR-062** — консолидация движков клиринга (ce_agent_clearing vs Engine B) — **открытый вопрос владельцу**.
- **ADR-063** — party_type как архитектурное решение.
- 🔧 **Корректировка:** ранняя гипотеза «ADR-050..053 отсутствуют» **опровергнута** — они существуют в репо; **следующий свободный номер = ADR-061**.

## 4. proto-contract-designer — контракты

- `AgentKind` enum; `AgentDelta{agent_id,agent_kind,asset,venue,delta,price_used,delta_value}` (замена `AssetDelta`); RPC `GetAgentPositions`; переформулировка `ExchangeNOP.z` без target; новый `transfer/v1` + топик `ce.transfers`.
- 🔧 **Находка:** `ApplyNodeTransferRequest`/`AssetDelta.venue` **уже есть** в proto — не дублировать, Э2 только добавляет `agent_id`.

## 5. data-schema-designer — схемы данных

- `ce_agent_position(agent_id,asset,venue) → position(signed), updated_at, last_batch_id`, upsert с guard по `last_batch_id`; `ce_transfer` (state machine `in_transit→delivered/timed_out→returned`).
- **party_type:** CLIENT/HOUSE — generated column в `accounts`; AGENT — отдельная таблица (несовместимые инварианты: у AGENT нет неотрицательности/резерва).
- `clearing_trace` → **ReplacingMergeTree** (текущий MergeTree ломает идемпотентность replay).
- `f05a_clearing_config` +5 колонок (ρ_Q/ρ_T/q_translator/q_arbitrageur/transfer_eta).
- 📌 **Подтверждено:** `agent_state`/`node_balances` (ADR-057/058) **никогда не создавались** в `init.sql` — миграция документационная. Предложен Flyway-lite runner (`schema_migrations` + `apply-migrations.sh`) — отдельным ADR, не блокирует.

## 6. frontend-architect — UI-спеки

- Вкладки на `/ce-treasury-live`, линия `σ*` на кривой, **6 TODO-контрактов** на данные.

## 7. test-architect — тест-план

- **66 проверок → 4 файла**, сценарии U1-U14.
- 🔧 **Флаг:** golden-источники `ce_tick_v2.py` (27 проверок) и `square_bands.py` (39) **отсутствуют в репо** — нужны от владельца для тест-goldens (см. §12).

## 8. trading-domain-specialist — доменные инварианты

- **A1 уже корректен** в коде; **A4 (π=x) корректен**. Пробелы: **A2/A6/A7/A8**.
- **10 инвариантов INV-CE-01..10**, в т.ч.: владение меняют ровно две вещи (клиент+исполнение); `x₊·x₋=0`; `W·c=0`; ранг `W=V·A−1`, ядро `(V−1)(A−1)`, `det W_нат=(p₂−p₁)/1000`.

## 9. cpp-service-architect — backend layering

- Разложение по `matching`/`ledger`/`risk` (transport/app/domain/infra) с точками `файл:функция`.
- **A2 (σ*=m+ρ·c)** — новая чистая функция `ce_sigma_shift.hpp` в matching-domain (не в assembler: там нет доступа к позициям), вызов **до** `AssembleCeGraph`.
- **ledger владеет книгой позиции** (§17): `ce_committed_`(per-asset) → порт `AgentPositionRepositoryPort` + PG `ce_agent_position` (**первая реальная PG-таблица ledger**).
- **Декремент позиции — при эмиссии (A7), не при исполнении (A8)**: ledger расширяет `RememberExecutionIntent` (idempotency по `intent_id`), risk не мутирует ledger напрямую (§10.3).
- **5 флагов**, все default OFF: `CE_V2_GRAPH → CE_AGENT_POS → CE_AGENT_BAND → CE_AGENT_A8 → CE_TRANSFER_AGENT`, снимаются независимо.

## 10. code-reviewer — ревизия текущего кода vs v2

- 🐛 **LIVE BUG:** `AssetDelta.venue` производится matching, но **отбрасывается консюмером** `kafka_consumers.cpp:62`; `ce_committed_` ключуется только по asset → нетит разнознаковые дельты по venue. Это причина, по которой узловой разрез мог не сходиться.
- **A2 anchor shift ОТСУТСТВУЕТ** в коде (не «двухслагаемый», как думали ранее) — ρ·c нигде не применяется.
- STOCK-плечо/book-узел — под удаление; A7-полоса **отсутствует**; `pos_delta_applied_` риск переполнения (cap 1000). Security на его слое — чисто.

## 11. security-reviewer — безопасность (2 CRITICAL)

- 🔴 **CRITICAL-1 [AUTH]:** `POST/GET/DELETE /v1/flow-orders` (`http_gateway.cpp:882-1010`) — **нет auth-middleware**; `user_id` берётся из тела запроса. Любой может действовать «за» произвольный `user_id`, включая будущий `__ce_house__`/`agent_id`. Строковый split CLIENT/AGENT/HOUSE в ledger защищён только строкой id → обходится с этого эндпоинта.
- 🔴 **CRITICAL-2 [KILL-SWITCH]:** `risk.PreHedgeCheck` **не подключён** к живому пути отправки (`cpp/venues/src/` не вызывает risk). `SetKillSwitch(halt=true)` не останавливает уже эмитированные заявки. Полоса/эмиссия v2 унаследует этот обход, как сегодняшний F-12.
- HIGH: insecure gRPC на risk (SetKillSwitch без actor/mTLS); hardcoded scale=8 в `to_dec` без overflow-guard; неограниченный рост `seen_execution_report_keys_`.
- MEDIUM-7: до Э4 отвергнутая/просроченная band-заявка оставляет `c_j` заниженной → **Э4 (возврат остатка) — жёсткий пререквизит включения `CE_AGENT_BAND`, не follow-up**.

## 12. implementation-planner — задачи T-F18-NNN

- **Docs-as-code группа** T-F18-001..014 (ADR-061 + ревизии, feature/UC/SEQ, proto, DDL, test-plan, env).
- **Э1** T-F18-101..103 (`CE_V2_GRAPH`) · **Э2** 201..205 (`CE_AGENT_POS`) · **Э3** 301..305 (`CE_AGENT_BAND`) · **Э4** 401..403 (`CE_AGENT_A8`) · **Э5** 501..505 (`CE_TRANSFER_AGENT`).
- Каждая кодовая таска: флаг OFF по умолчанию, зелёный `make test-ci`, независимый `git revert`.
- **Переиспользовать, не переизобретать:** thin-venue фильтр (`2e4c86b2`), A1/`BuildQuoteAgent`, клиринг-ядро `ClearCe`, `GetNodeBalances`/`ApplyNodeTransfer`, ADR-060, настраиваемая комиссия (`d4900700`).

---

## 13. Межагентный консенсус — единый блокирующий гейт владельца

Реализация Э1 **не стартует**, пока владелец не подтвердит (план §2):

1. **Номер фичи** — заголовок плана фиксирует «F-18 эволюционирует», но §2.1 тела ещё спрашивает F-19/F-21. → снять явным ответом. *(F-18 уже переименован в feature-index — консистентно.)*
2. **ADR-061** свободен (ADR-050..053 существуют) → перевести в `accepted` после решений.
3. **Консолидация движков** ce_agent_clearing vs Engine B (ADR-062).
4. **AGENT — третий party_type**, не подтип клиента (ADR-063).
5. **security CRITICAL-1/2** — до Э1 либо починить (gateway auth; PreHedgeCheck в путь venues), либо явно принять как отдельный отслеживаемый риск с компенсирующим контролем.

## 14. Сводные корректировки (что оказалось не так, как думали)

| Ранее считалось | Факт (по агентам) |
|---|---|
| ADR-050..053 отсутствуют | существуют → next free = **ADR-061** |
| A2 anchor — «двухслагаемый» в коде | **отсутствует** (ρ·c нигде не применён) |
| Узловой разрез сходится | 🐛 `AssetDelta.venue` **отбрасывается** консюмером — live bug |
| `agent_state`/`node_balances` есть в БД | **никогда не создавались** в init.sql |
| Э4 (A8) — follow-up | **пререквизит** включения `CE_AGENT_BAND` (security MEDIUM-7) |
| Kill-switch останавливает хедж | **не останавливает** — PreHedgeCheck не в пути (CRITICAL-2) |

## 15. Отложенные входы (нужны от владельца)

- `ce_tick_v2.py` (27) + `square_bands.py` (39) — golden-источники для тест-батареи (T-F18-012).
- Решения §2 (гейт §13) — до ADR-061 `accepted`.

---

## Отчёт по изменениям

- **Создан:** этот файл (`docs/implementation-plan/F-18-v2-agents-review-summary.md`) — сводка 12 агентов.
- **Обновлений контрактов/кода:** нет (сводка, no-code-before-docs соблюдён).
- **Команды сборки/тестов:** не запускались (docs-only артефакт).
- **Риски/следующие шаги:** гейт владельца §13; корректировки §14; отложенные входы §15. Реализация — только после `accepted` ADR-061.
