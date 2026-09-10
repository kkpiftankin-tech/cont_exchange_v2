---
id: DOC-TEST-F-05A-CE-AGENTS
phase: 10-testing
status: draft
owner: core-team
source:
  - CE_algorithm_spec.md §7 (тесты), §A5 (инварианты)
  - CE_virtual_counterparties.md §7 (эталонные прогоны), §13/§14 (Python-эталон)
related:
  - docs/02-system/use-cases/UC-F05A-06-ce-agent-tact-clearing/use-case.md
  - docs/03-architecture/adr/ADR-055-ce-virtual-counterparties-agents.md
  - docs/03-architecture/adr/ADR-056-ce-node-graph-clearing.md
  - docs/03-architecture/adr/ADR-057-ce-three-level-position.md
  - docs/implementation-plan/F-05A-CE-agents.tasks.md
---

# F-05A CE Virtual Counterparties — план тестирования

> Легенда: ✅ выполнено, ⚠ частично, ❌ не выполнено. Всё ❌ (реализация не начата).
>
> **Главный принцип:** эталон — Python-реализация того же алгоритма
> (`agents_vc.py`, `agents_sim.py`, ⧗ не приложены к запискам — нужны от владельца).
> Требование: C++ и Python на одних входах дают одни потоки/позиции с точностью **1e-9**.

## Раздел 1. Unit-тесты (U-CEA-001..012)

Чистая доменная логика без Kafka/живых venue. GTest. Файлы —
`cpp/market_data/tests/` (агенты/узлы) и `cpp/matching/tests/` (клиринг/инварианты).

| # | Тест | Цель / инвариант | ADR |
| --- | --- | --- | --- |
| U-CEA-001 | AgentBuilderDeterministic | снапшот → агенты `(α,c,σ*)`; повтор даёт тот же результат | ADR-055 |
| U-CEA-002 | DeadZoneCurve | `f(σ)=α·sign(σ*−σ)·max(0,|σ*−σ|−c)`; в полосе `|σ−σ*|≤c` поток 0 | ADR-055 |
| U-CEA-003 | AgentToTwoSegments | агент → 2 односторонних сегмента со скоростями `σ*∓c` | ADR-055 |
| U-CEA-004 | ClosedFormPnL | `PnL=½Σα(|d|−c)²₊` == прямой подсчёт (доход−проскальзывание−издержки) | ADR-055 |
| U-CEA-005 | BuildNodeBasis | базис узлов `ASSET@VENUE` + `__book__`; стабильный порядок | ADR-056 |
| U-CEA-006 | BookNodeFree | узлы книги не получают строк `W`; несут марку μ в лин.члене | ADR-056 |
| U-CEA-007 | InventoryBox | box запаса `l=−q,u=+q` по узлам книги; позиция ≠ 0 при разных mid | ADR-056 |
| U-CEA-008 | MarkWeighted | `μ_a = Σ α_j m_{a,j} / Σ α_j` (средневзвешенный по глубине mid) | ADR-056 |
| U-CEA-009 | ZaFormula | `Z_a = Σ_venue qty + in_transit + committed − target` | ADR-057 |
| U-CEA-010 | AggregateOverride | `|Z_a|>z_limit` → сдвиг скоростей `−ρ_lim·(Z_a−sign·z_limit)` | ADR-057 |
| U-CEA-011 | CommittedFreeVolume | клиринг видит `свободное = qty − committed` | ADR-058 |
| U-CEA-012 | PositionIdentity | `ΔP_a = Σ_j f(stock a@j) = −Σ_j f(quote a@j)` (Ф4, тремя способами) | ADR-057 |

## Раздел 2. Golden vs Python-эталон (G-CEA-001..008)

C++↔Python до 1e-9 на одних входах (базовый набор параметров §14 записки).

| # | Сценарий | Ожидание |
| --- | --- | --- |
| G-CEA-001 | Базовый такт (3 площадки, mid Binance 0 / OKX +0.35 / Kraken −0.20 ‰) | потоки и позиции совпадают до 1e-9; PnL +0.00114 USDT |
| G-CEA-002 | Разрыв < Σ мёртвых зон маршрута | все потоки нули (агенты молчат) |
| G-CEA-003 | Разрыв чуть выше порога | торгуют только две ноги из шести |
| G-CEA-004 | **Без связок и без запаса** | оборот котировок ровно 0 (переводчики не могут торговать) |
| G-CEA-005 | **Без плеч запаса** | `ΔP≡0` при ненулевом обороте (позиция ≡ 0 — Wx=0 частный случай) |
| G-CEA-006 | Без связок (есть запас) | весь оборот → позиция (`−0.048` при обороте `0.048`) |
| G-CEA-007 | Смещённая марка `μ±δ` | линейный дрейф позиции; знак: марка выше рынка → биржа копит | ADR-056 |
| G-CEA-008 | Полный набор | в позицию уходит ~9% оборота; остальное проходит насквозь |

## Раздел 3. Инварианты как стадия конвейера (V-CEA-001..006)

Проверяются в `matching` после `Solve`, до эмиссии (ADR-059). Нарушение И-Т1/И-Т3 →
такт `FAILED`, заявки не эмитятся, событие в `risk.alerts`.

| # | Инвариант | Условие |
| --- | --- | --- |
| V-CEA-001 (И-Т1) | баланс узлов | `max_u |(Wx)_u| < ε` (ε=1e-9 в тыс. USDT) |
| V-CEA-002 (И-Т2) | PnL двумя способами | `Σ f·d − Σ f²/(2α) − Σ c|f| = ½ Σ α(|d|−c)²₊` |
| V-CEA-003 (И-Т3) | позиция тремя способами | `Σ_j f(stock a@j) = −Σ_j f(quote a@j)` |
| V-CEA-004 (И-Т4) | исполнимость | `Q_{a,j} + Δ_немедленное ≥ 0` по каждому узлу |
| V-CEA-005 (И-Т5) | дрейф марки | скользящее среднее `ΔZ_a` около нуля (иначе чинить оценщик μ) |
| V-CEA-006 (И-Т6) | агрегатный лимит | `|Z_a + ΔZ_a| ≤ z_limit` |

## Раздел 4. Integration / E2E (E-CEA-001..004)

| # | Сценарий | Проверка |
| --- | --- | --- |
| E-CEA-001 | Цепочка такта | market-data(агенты) → matching(клиринг+инварианты) → `ce.position.delta` → ledger(узлы) → risk(`Z_a`) → venues; `clearing_trace` заполнена |
| E-CEA-002 | Регресс двойной отправки | два такта без подтверждений: суммарный назначенный объём по агенту ≤ `capacity` (ADR-058) |
| E-CEA-003 | Неисполнимая нога (И-Т4) | нога режется/закрывается по рынку; позиция консистентна |
| E-CEA-004 | Пере-запуск ledger | узловые остатки восстанавливаются; позиция не теряется |

## Used In

- Feature: [F-05A](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)
- Use case: [UC-F05A-06](../../02-system/use-cases/UC-F05A-06-ce-agent-tact-clearing/use-case.md)
- Tasks: [F-05A-CE-agents.tasks.md](../../implementation-plan/F-05A-CE-agents.tasks.md) (T-CEA-501/502)
