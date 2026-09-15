<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
feature: F-18
supersedes: [UC-F18-01, UC-F18-02, UC-F18-03]
parentRequirements: [ADR-061]
---
-->

# UC-F18-04. Такт клиринга виртуальных контрагентов и хедж по полосе ±q (v2)

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-18 (v2)](../../features/F-18-ce-capital-position-hedge/) |
| ☁️ L0 system sequence | [SEQ-UC-F18-04-system](sequences/SEQ-UC-F18-04-system.md) |
| 🌊 L1 service sequence | [SEQ-F18-UC-F18-04-services](../../../05-components/sequences/SEQ-F18-UC-F18-04-services.md) |
| 💻 Source code | matching / ledger / risk (planned под ADR-061) |

## Feature

- [F-18 (v2). CE Virtual Counterparties — Translator/Arbitrageur Agents, Signed Agent Position & Band Hedge](../../features/F-18-ce-capital-position-hedge/)

## Supersedes (v1)

Этот UC v2 заменяет модель v1. Per CLAUDE.md §0a (Conflict rule) — не «молчаливая» замена:

- [UC-F18-01](../UC-F18-01-form-ce-capital/use-case.md) — form CE capital (seed + PnL) → house-невязка.
- [UC-F18-02](../UC-F18-02-derive-ce-position/use-case.md) — net position (проекция x) → per-agent signed `c_j`.
- [UC-F18-03](../UC-F18-03-hedge-net-position/use-case.md) — hedge по θ_a → per-agent полоса ±q_j.

## Primary Actor

System (плановый такт виртуальных контрагентов CE, триггерится оконным снимком стаканов).

## Supporting Actors

- Liquidity Trader (Client) — источник встречного потока (сделка меняет владение немедленно).
- External Venue (CEX: Binance/Kraken) — площадка исполнения хедж/перевозочных заявок.
- Operator — наблюдатель (позиции агентов, house, `Z_a` в UI).

## Preconditions

- Конфигурация агентов задана: `ρ_Q=0,004`, `ρ_T=0,002`, полки/тарифы, полосы
  `q_translator=18`, `q_arbitrageur=30`; house-столбец сконфигурирован.
- Позиция агента `ce_agent_position` доступна из ledger (может быть нулевой на холодном старте).
- Флаги v2 включены (`CE_V2_GRAPH`, `CE_AGENT_POS`, `CE_AGENT_BAND`, `CE_AGENT_A8`);
  `CE_AGENT_BAND` требует `CE_AGENT_A8` (возврат остатка) как пререквизит.

## Trigger

Market-data публикует оконный снимок обоих стаканов с единым `batch_id` (A0).

## Main Flow

1. **A0.** Снимок обоих стаканов на единый момент такта (внутри такта не меняется).
2. **A1.** Для каждого переводчика/арбитражёра строятся три числа кривой:
   якорь `m = 1000·ln(mid/P₀)`, наклон `α = θ·min V/d`, полка `c = комиссия + ½·|ln(ask/bid)|·1000`.
3. **A2.** Сдвиг якоря `σ*_j = m_j + ρ_j · c_j` — единственное слагаемое, от СОБСТВЕННОЙ
   позиции агента (видит `c_j` до накопления такта).
4. **A3.** Сборка матрицы баланса `W` (узлы `актив@площадка` × намерения агентов) + столбец house.
5. **A4.** Клиринг `max_f Σ[−σ*_j·f_j − c_j·|f_j| − f_j²/(2α_j)]` при `W·f=0`;
   закрытая форма (квадрат `λ*`) сверяется с OSQP.
6. **A5.** Проверки перед действием: `|Wf|<1e-6`, согласие двух решателей,
   исполнимость (объём заявки ≤95% остатка узла-источника).
7. **A6.** Накопление `c_j ← c_j + f_j` (полный шаг). Наружу не идёт ничего;
   владение узлов не меняется; `c_j` может стать отрицательной.
8. **A7 (полоса).** Для каждого агента: если `|c_j| ≤ q_j` — ничего; иначе
   отправить наружу `(|c_j|−q_j)·sign(c_j)`, размер по стоимости → qty итеративным
   walk по книге (2–3 итерации), `PreHedgeCheck` (reuse F-12), пометить `in_flight` и
   уменьшить `c_j` НЕМЕДЛЕННО (в момент эмиссии, не по confirm).
9. **A8 (подтверждения).** Исполнение меняет только `qty[asset@venue]` узла и
   `realized = fq·(марка−px_факт)/1000·sgn`; позицию агента confirm не трогает.
10. Публикация `ClearingTrace` (`batch_id`, `agent_id`, `c_j` до/после, house-невязка).

## Alternative Flows

- **A-1. Пустой клиринг круга.** `|S| ≤ C` для круга → поток по нему нулевой (круг
  убыточен), такт продолжается для остальных кругов/агентов — не ошибка.
- **A-2. Позиция внутри полосы.** `|c_j| ≤ q_j` → эмиссии нет, позиция копится дальше.
- **A-3. Таймаут in_flight.** Неисполненный к дедлайну остаток возвращается в `c_j` по
  ПЛАНОВОЙ цене (цене эмиссии), не по марке на момент таймаута; realized — только от
  исполненной доли.

## Negative Flows

- **N-1. Нарушение инварианта.** `|Wf| ≥ 1e-6` ИЛИ закрытая форма ≠ OSQP → такт `FAILED`,
  заявки не эмитятся, `risk.alerts`, трасса помечена `failed`.
- **N-2. Неисполнимость ноги.** Объём > 95% остатка узла-источника → нога урезается/
  обнуляется для этого такта.
- **N-3. PreHedgeCheck REJECT.** Intent не публикуется, `c_j` НЕ уменьшается (остаётся
  за полосой, повтор в следующем такте), `risk.alert`.

## Postconditions

- `c_j` обновлён у всех агентов (знаковый, с нуля исторически); при эмиссии — уменьшен
  ровно на отправленное.
- Владение биржи изменилось ТОЛЬКО клиентской сделкой (A0) и/или исполнением (A8);
  `Z_a = владение_a(t) − владение_a(0)`; без клиентского потока `Z_a ≡ 0`.
- **INV-CE-A0:** позицию агента `c_j` двигают встречные потоки клиринга — клиентский
  встречный поток (A0, когда клиенты есть) и/или встречные потоки переводчиков и арбитражёров
  между разными внешними площадками (собственное накопление клиринга). Клиентов может не
  быть — тогда A0 нет, но `c_j` двигают межплощадочные встречные потоки агентов; неподвижно
  без клиентов/исполнения только владение `Z` (`Z≡0`), а не `c_j`. Остаток STOCK `c_j` не
  двигает (его нет).
- Прибыль/невязка осела на счёте дома (house); house заявок не порождал.
- `ClearingTrace` опубликован с `batch_id` — цепочка order→batch→position→house.

## Acceptance Criteria

Покрывает AC-F18v2-01..18 из [feature.yaml](../../features/F-18-ce-capital-position-hedge/feature.yaml).
Ключевые: полоса ±q (07..11), инвариант владения и `Z_a` (13, 14), party_type AGENT (16),
house (15), эффект полосы −63% с оговоркой (18).

## Related Sequence Diagrams

- [SEQ-UC-F18-04-system](sequences/SEQ-UC-F18-04-system.md) (L0 ☁️)
- [SEQ-F18-UC-F18-04-services](../../../05-components/sequences/SEQ-F18-UC-F18-04-services.md) (L1 🌊)

## Related Contracts

- Kafka: `ce.position.delta` (знаковая Δc_j агента), `execution.intents` (заявка сверх полосы),
  `execution.venue` (ExecutionReport, A8 confirm).
- gRPC: `fob.ledger.v1.LedgerService.GetAgentPositions`, `fob.risk.v1.RiskService.PreHedgeCheck`
  (planned под ADR-061).

## Related Components

- [matching-fob-core](../../../05-components/matching-fob-core/overview.md), [ledger](../../../05-components/ledger/overview.md),
  [risk-manager](../../../05-components/risk-manager/overview.md), [external-venues](../../../05-components/external-venues/overview.md)

## Related Data

- PostgreSQL: `ce_agent_position(agent_id, asset, venue) → position(signed)`,
  `f05a_clearing_config` (+`q_translator`, `q_arbitrageur`), `ce_transfer` (planned).

## Source Fragments

- `incoming-docs/2026-09-15-CE_algorithm_v2.md` §A0–A8, §«Полоса», §«Позиция биржи по каждой валюте», §«Счёт дома».
- ADR-061 (planned, supersede ADR-054/057; ревизии ADR-055/056/058).
