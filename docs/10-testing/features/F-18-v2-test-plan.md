---
id: DOC-TEST-F-18-V2
phase: 10-testing
status: draft
owner: core-team
level: sea
source:
  - incoming-docs/2026-09-15-CE_algorithm_v2.md (A0–A8, «Позиция биржи», «Счёт дома», «Эффект полосы»)
  - docs/implementation-plan/CE-v2-agents-rework.plan.md §8 (батарея тестов)
  - docs/implementation-plan/F-18-v2-agents-review-summary.md §7 (test-architect), §15 (отложенные входы)
related:
  - docs/implementation-plan/F-18-v2.tasks.md (T-F18-012, T-F18-505)
  - docs/03-architecture/adr/ADR-061-ce-v2-agent-position-band-hedge.md (планируется)
  - docs/10-testing/reference/ce-agents/README.md (прецедент ingestion Python-эталона → golden)
  - docs/10-testing/features/F-05A-CE-agents-test-plan.md (прецедент структуры)
---

# F-18 v2 — план тестирования: CE-агенты (переводчики + арбитражёры), позиция с нуля, полоса ±q

> Легенда: ✅ выполнено, ⚠ частично, ❌ не выполнено, 🔒 заблокировано входом владельца.
> Всё ниже — ❌ (реализация Э1 не начата, гейт владельца не снят — см. `F-18-v2.tasks.md §Блокирующий гейт`).

## 0. Предпосылки и блокеры (читать до переноса тестов)

### 0.1. 🚩 Golden-источники ОТСУТСТВУЮТ в репозитории

**`ce_tick_v2.py` (27 проверок) и `square_bands.py` (39 проверок) физически отсутствуют**
в `incoming-docs/`, в `docs/10-testing/reference/ce-agents/` и где-либо ещё в репо. Сейчас
в `docs/10-testing/reference/ce-agents/` лежат только `agents_vc.py`/`agents_sim.py` +
`golden-vc.txt`/`golden-sim.txt` от F-05A v1 — это **не** те источники.

- Оба файла **нужны от владельца**.
- Регистрация — **отдельным вызовом skill `ingest-docs`** (не частью этой таски и не частью
  T-F18-012), по тому же паттерну, что уже применён для F-05A:
  `agents_vc.py → golden-vc.txt`, `agents_sim.py → golden-sim.txt`
  (см. `docs/10-testing/reference/ce-agents/README.md`).
- До регистрации golden оригиналы `incoming-docs/` **immutable**; числовые допуски
  калибруются **по факту ingestion**.

**Следствие для распределения 27+39 → 66.** Разбивка по конкретным `def test_*` ниже —
**оценочная**, восстановлена по структуре алгоритма A0–A8 и по числам, уже зафиксированным
в записке (пример Binance `α=35,986`; матрица `W` 2×2; таблица «эффект полосы за 30 тактов»).
При фактической регистрации возможна перебалансировка внутри 4 файлов; **набор из 4 файлов
и сумма 66 не меняются**.

### 0.2. Прочие блокеры

1. **Формальных AC ещё нет** — `feature.yaml` F-18 v2 не создан (T-F18-005). Тесты замэплены
   на пункты алгоритма **A0–A8** как суррогат AC-ID (`AC-A1`…`AC-A8`). После появления
   `feature.yaml`/ADR-061 таблицу AC→Test перелинковать на реальные AC-ID (правка docs, не тестов).
2. **Стиль тестов — НЕ GTest.** Фактический код в `cpp/matching/tests/domain/*` —
   самодельный `int main()` + локальная `bool close(a,b,tol,what)` с `++g_fail`,
   `printf("FAIL ...")`, возврат `0`/`1`. Задача просит «GTest-имена» — ниже они даны в форме
   `Suite::Case` как **логические идентификаторы кейсов**; при реализации это самодельные
   `main()`-тесты (по образцу `ce_agent_clearing_test.cpp`), а `Suite::Case` — имя блока внутри.
3. **Движок v2 — новый заголовок под флагом.** v2 убирает `CeLeg::kStock` и book-узел,
   добавляет house-столбец → сигнатуры v1 переиспользуются не один-в-один. Тест-план написан
   от алгоритма A0–A8, имена типов сверяются заново при реализации Э1.
4. **Реальный CI-гейт — `make test-ci`** (изолированный Docker, полный `ctest`).
   `ctest` в `.github/workflows/cpp-build.yml` сейчас `continue-on-error: true` — **не гейт**.

---

## 1. Инварианты, проверяемые батареей (обязательны в тестах)

| # | Инвариант | Где проверяется | A-раздел |
|---|---|---|---|
| INV-1 | **Владение меняют РОВНО две вещи** — сделка с клиентом и исполнение на площадке. Клиринг, накопление позиций и перевозы владение НЕ меняют никогда. `Z_a=владение_a(t)−владение_a(0)`, старт с нуля. Без клиентского потока `Z≡0`. | U10, U13, U14; `agent_band_test::OwnershipInvariant`; E2E-2 | «Позиция биржи», A6, A8 |
| INV-2 | **`x₊·x₋=0`** (комплементарность) — на одном ребре не бывает одновременно ненулевой покупки и продажи. Точность 1e-12. | U8; `vector_qp_pi_test::ComplementarySlackness` | A4 |
| INV-3 | **`W·c=0`** — накопленная позиция лежит в ядре баланса; базис ядра `(+1,−1,+1,+1)`, одна компонента противоположна ⇒ отрицательная позиция обязательна (`c≥0` даёт только `c=0`). | U10; `agent_band_test::KernelPreserved`, `NegativePositionRequired` | A6 |
| INV-4 | **ранг `W = V·A−1`**; правое ядро `(V−1)(A−1)`; левое ядро одномерно, вектор `(1,…,1)` (свобода нумерария, закрепляется `π[USD@Binance]=0`). | U4, U7; `w_structure_test::RankSquare`, `RightKernelDim`, `LeftKernelOne` | A3 |
| INV-5 | **`det W_нат = (p₂−p₁)/1000`** в натуральных единицах (2 площадки × 2 актива); вырождается при равных ценах; house-столбец восстанавливает ядро. Точность 1e-9. | U5; `w_structure_test::DetWNatural`, `HouseColumnRestoresKernel` | A3, «Счёт дома» |
| INV-6 | **После эмиссии `|c_j| = q_j` ТОЧНО** (не `≤` с допуском): при `|c|>q` наружу уходит `(|c|−q)·sign(c)`, на столько же уменьшается `c`. | U11; `agent_band_test::EmissionExactBand` | A7 |
| INV-7 | **Сдвиг якоря — одно слагаемое** `σ*=m+ρ·c` (нет второго члена `ρ(Q−Q*)`; `ρ_Q=0,004`, `ρ_T=0,002`). | U3; `three_numbers_test::AnchorShiftSingleTerm` | A2 |
| INV-8 | **Возврат по таймауту — по ПЛАНОВОЙ цене** (не рыночной/не марочной); поздний филл после таймаута не двоит возврат. | U13; `agent_band_test::TimeoutReturnsAtPlannedPrice` | A8 |
| INV-9 | **Прибыль — на счёт дома по ФАКТУ расчёта** (не по клирингу): `realized += fq·(марка−px_факт)/1000·sgn`; позицию агента при исполнении НЕ трогаем (уменьшена на A7). | U13; `agent_band_test::HouseProfitOnFact` | A8, «Счёт дома» |
| INV-CE-A0 | **Позицию агента `c_j` двигают встречные потоки клиринга:** клиентский встречный поток (A0, когда клиенты есть) И/ИЛИ встречные потоки **переводчиков и арбитражёров между разными внешними площадками** (собственное накопление клиринга, A6). STOCK-остаток позицию НЕ двигает (его нет). **Клиентов может не быть** — тогда A0 отсутствует, но `c_j` продолжают двигать межплощадочные встречные потоки агентов. Отличие от INV-1: без клиентов/исполнения неподвижно **владение `Z`** (`Z≡0`), **а не** позиция `c_j`. | U13; `agent_band_test::InterAgentCounterFlowDrivesPosition`, `PositionNotDrivenByStock` | A0, A6, ADR-061 §3 |

---

## 2. Сценарии U1–U14 (доменные группы, ось A0–A8)

Каждый сценарий = группа проверок из батареи. Столбец «Источник» — предполагаемый golden-файл.

| Сценарий | Что моделирует | A-раздел | Источник | GTest-файл |
|---|---|---|---|---|
| **U1** | Три числа из стакана: якорь `m`, наклон-минорант `α`, полка `c` | A1 | `ce_tick_v2.py` | `three_numbers_test` |
| **U2** | Кривая `f(σ)` из трёх кусков (продажа/полка/покупка) + непрерывность на `m±c` | A1 | `ce_tick_v2.py` | `three_numbers_test` |
| **U3** | Сдвиг якоря от собственной позиции `σ*=m+ρ·c` (одно слагаемое) | A2 | `ce_tick_v2.py` | `three_numbers_test` |
| **U4** | Структура матрицы `W`: ранг, левое/правое ядро, счёт агентов | A3 | `square_bands.py` | `w_structure_test` |
| **U5** | `det W_нат=(p₂−p₁)/1000`, вырождение при равных ценах, house-столбец восстанавливает ядро | A3, «Счёт дома» | `square_bands.py` | `w_structure_test` |
| **U6** | Клиринг — закрытая форма `λ*` (три ветви `S<−C` / `S>+C` / `|S|≤C`) | A4 | `ce_tick_v2.py` | `vector_qp_pi_test` |
| **U7** | Gauge-инвариантность цен `π`; фиксация `π[USD@Binance]=0` → единственность | A3, A4 | `ce_tick_v2.py` | `vector_qp_pi_test` |
| **U8** | Комплементарность `x₊·x₋=0`; мёртвая зона `|S|≤C` → нулевой поток (пустой такт) | A4, A5 | `ce_tick_v2.py` | `vector_qp_pi_test` |
| **U9** | Проверки перед действием: узловой баланс `|Wf|<1e-6`; согласие closed-form vs OSQP | A5 | `ce_tick_v2.py` | `vector_qp_pi_test` |
| **U10** | Накопление `c←c+f`; `W·c=0`; отрицательная позиция обязательна; минимум прогона `≈−18000` | A6 | `ce_tick_v2.py`+`square_bands.py` | `agent_band_test` |
| **U11** | Полоса: эмиссия только при `|c|>q`; после эмиссии `|c|=q` точно | A7 | `ce_tick_v2.py` | `agent_band_test` |
| **U12** | Размер заявки по стоимости `qty←излишек·1000/walk(qty)`; сходимость 2–3 итераций | A7 | `ce_tick_v2.py` | `agent_band_test` |
| **U13** | A8: возврат по таймауту по плановой цене; признание прибыли по факту; поздний филл не двоит; инвариант владения | A8 | `ce_tick_v2.py` | `agent_band_test` |
| **U14** | 30-тактовый прогон: эффект полосы `−63%` издержек; переводчик `q=18` vs арбитражёр `q=30`; полоса без клиентского потока УХУДШАЕТ (задокументировано) | «Эффект полосы» | `square_bands.py` | `agent_band_test` |

---

## 3. Батарея 66 проверок → 4 файла (`cpp/matching/tests/domain/`), по этапам Э1–Э4

Распределение: `ce_tick_v2.py` (27) = `three_numbers`(8) + `vector_qp_pi`(7) + `agent_band`(12);
`square_bands.py` (39) = `w_structure`(12) + `agent_band`(27). Итого **8+7+12+12+27 = 66**.

| Файл | Этап (флаг) | Источник (оценка) | Кол-во |
|---|---|---|---|
| `three_numbers_test.cpp` | Э1 (`CE_V2_GRAPH`) | `ce_tick_v2.py` A1/A2 | 8 |
| `vector_qp_pi_test.cpp` | Э1 (`CE_V2_GRAPH`) | `ce_tick_v2.py` A4/A5 | 7 |
| `w_structure_test.cpp` | Э1 (`CE_V2_GRAPH`) | `square_bands.py` (структурная часть) | 12 |
| `agent_band_test.cpp` | Э3 (`CE_AGENT_BAND`) + Э4 (`CE_AGENT_A8`) | `ce_tick_v2.py` A6/A7/A8 (12) + `square_bands.py` (числовые прогоны, 27) | 39 |

> Регистрация в `cpp/matching/CMakeLists.txt` — по образцу блока `matching_ce_clearing_test` /
> `matching_ce_assembler_test` / `matching_ce_projection_test` (внутри `if(BUILD_TESTING)`,
> `add_executable`+`target_include_directories`+`add_test`). Header-only домен без внешних
> зависимостей, кроме `vector_qp_pi_test` — ему нужен `osqp_backend` (уже линкуется в
> `matching_osqp_backend_test`) для сравнения closed-form vs OSQP.

### 3.1. `three_numbers_test.cpp` — Э1 — A1/A2 (CHK-01..08)

| ID | GTest-имя (логическое) | Given → Expected | U | AC |
|---|---|---|---|---|
| CHK-01 | `three_numbers_test::AnchorFromMid` | стакан Binance → `m=1000·ln(mid/P₀)=+0,0001‰` | U1 | AC-A1 |
| CHK-02 | `three_numbers_test::SlopeMinorant` | `α=θ·min V/d`, `θ=0,50` → `α=0,50·71,971=35,986` (tol 1e-3); линия `α·d` не выше ломаной | U1 | AC-A1 |
| CHK-03 | `three_numbers_test::ShelfFromFeeSpread` | `c=комиссия+½·|ln(ask/bid)|·1000 = 0,20+0,3333=0,5333‰` | U1 | AC-A1 |
| CHK-04 | `three_numbers_test::CurvePieceSell` | `σ>m+c` → `f=α·(σ−m−c)` | U2 | AC-A1 |
| CHK-05 | `three_numbers_test::CurvePieceShelf` | `|σ−m|≤c` → `f=0` | U2 | AC-A1 |
| CHK-06 | `three_numbers_test::CurvePieceBuy` | `σ<m−c` → `f=α·(σ−m+c)` | U2 | AC-A1 |
| CHK-07 | `three_numbers_test::CurveContinuity` | `f(m+c)=0`, `f(m−c)=0`, `f((m+c)+ε)=α·ε` — без скачка | U2 | AC-A1 |
| CHK-08 | `three_numbers_test::AnchorShiftSingleTerm` | `c_j=+5`, `ρ_Q=0,004` → `σ*=m+0,02`; **нет** члена `ρ(Q−Q*)` (INV-7) | U3 | AC-A2 |

### 3.2. `vector_qp_pi_test.cpp` — Э1 — A4/A5 (CHK-09..15)

| ID | GTest-имя | Given → Expected | U | AC |
|---|---|---|---|---|
| CHK-09 | `vector_qp_pi_test::GaugeInvariance` | сдвиг всех `π` на `k` → потоки `f` (по `σ=Wᵀπ`) не меняются | U7 | AC-A3 |
| CHK-10 | `vector_qp_pi_test::GaugeFix` | фиксация `π[USD@Binance]=0` → решение единственно | U7 | AC-A3 |
| CHK-11 | `vector_qp_pi_test::SigmaFromWtPi` | `σ=Wᵀπ` — знак корректен | U6 | AC-A4 |
| CHK-12 | `vector_qp_pi_test::ClosedFormBranchLow` | `S<−C` → `λ*=−(S+C)/(2A)` | U6 | AC-A4 |
| CHK-13 | `vector_qp_pi_test::ClosedFormBranchHigh` | `S>+C` → `λ*=−(S−C)/(2A)` | U6 | AC-A4 |
| CHK-14 | `vector_qp_pi_test::ClosedFormVsOsqp` | квадрат (`V=2,A=2`, 4 агента): `λ*_closed == λ*_osqp`, tol 1e-9 | U9 | AC-A5 |
| CHK-15 | `vector_qp_pi_test::NodeBalance` | `max_u|Wf|_u<1e-6` (v2 закладывает запас до 1e-9) | U9 | AC-A5 |

Дополнительно в этом файле проверяются INV-2/INV-8-соседи (переезжают в U8, помечены в §3.4):
`ComplementarySlackness` (`x₊·x₋=0`) и `DeadZoneNoFlow` (`|S|≤C` → `f=0`) — учтены в квоте
7 как часть A4/A5 (при ingestion могут распределиться между CHK-11..15).

### 3.3. `w_structure_test.cpp` — Э1 — A3 / «Счёт дома» (CHK-16..27)

| ID | GTest-имя | Given → Expected | U | AC |
|---|---|---|---|---|
| CHK-16 | `w_structure_test::RankSquare` | `V=2,A=2` → `rank(W)=V·A−1=3` (INV-4) | U4 | AC-A3 |
| CHK-17 | `w_structure_test::RightKernelDim` | правое ядро (независимые круги) `(V−1)(A−1)=1` | U4 | AC-A3 |
| CHK-18 | `w_structure_test::LeftKernelOne` | левое ядро одномерно, базис `(1,1,1,1)` | U4 | AC-A3 |
| CHK-19 | `w_structure_test::KernelVector` | `k=(+1,−1,+1,+1)` → `W·k=0` | U4 | AC-A3 |
| CHK-20 | `w_structure_test::AgentCount` | всего агентов `2VA−V−A` | U4 | AC-A3 |
| CHK-21 | `w_structure_test::TranslatorCount` | переводчиков `V·(A−1)` | U4 | AC-A3 |
| CHK-22 | `w_structure_test::ArbitrageurCount` | арбитражёров `A·(V−1)` | U4 | AC-A3 |
| CHK-23 | `w_structure_test::DetWNatural` | `det W_нат=(p₂−p₁)/1000`, tol 1e-9 (INV-5) | U5 | AC-A3 |
| CHK-24 | `w_structure_test::DegenerateEqualPrices` | равные цены → `det W_нат=0` (вырождение) | U5 | AC-A3 |
| CHK-25 | `w_structure_test::HouseColumnRestoresKernel` | house-столбец в узле нумерария → `W` обратима | U5 | «Счёт дома» |
| CHK-26 | `w_structure_test::HouseSingleNonzeroCell` | у house ровно одна ненулевая клетка (узел нумерария); нет якоря/наклона/полки/полосы | U5 | «Счёт дома» |
| CHK-27 | `w_structure_test::HouseTakesArbProfit` | house забирает `= p_Kraken−p_Binance`, остаток сметается в нумерарий без остатка | U5 | «Счёт дома» |

### 3.4. `agent_band_test.cpp` — Э3 (`CE_AGENT_BAND`) + Э4 (`CE_AGENT_A8`) — A6/A7/A8 + числовые прогоны (CHK-28..66)

**Часть ce_tick_v2.py — A6/A7/A8 (CHK-28..39, 12):**

| ID | GTest-имя | Given → Expected | U | AC |
|---|---|---|---|---|
| CHK-28 | `agent_band_test::Accumulation` | серия тактов → `c←c+f` per-agent | U10 | AC-A6 |
| CHK-29 | `agent_band_test::KernelPreserved` | после накопления `W·c=0` (INV-3) | U10 | AC-A6 |
| CHK-30 | `agent_band_test::NegativePositionRequired` | требование `c≥0` допускает только `c=0` (нет оборота) | U10 | AC-A6 |
| CHK-31 | `agent_band_test::NegativePositionMinimum` | 30-тактовый прогон: минимум `≈−18000` тыс. (sanity до golden) | U10 | AC-A6 |
| CHK-32 | `agent_band_test::EmissionThreshold` | `|c|≤q` → эмиссии нет | U11 | AC-A7 |
| CHK-33 | `agent_band_test::EmissionExactBand` | `c=25,q=18` → наружу `7`, после `c=18` **точно** (INV-6) | U11 | AC-A7 |
| CHK-34 | `agent_band_test::EmissionReducesPosition` | `c` уменьшается ровно на отправленное (нет повторной отправки) | U11 | AC-A7 |
| CHK-35 | `agent_band_test::EmissionSizingByValue` | `qty←излишек·1000/walk(qty)` — формула размера по стоимости | U12 | AC-A7 |
| CHK-36 | `agent_band_test::EmissionSizingConverges` | 2–3 итерации фиксированной точки сходятся; вырожденная кривая не зацикливается | U12 | AC-A7 |
| CHK-37 | `agent_band_test::TimeoutReturnsAtPlannedPrice` | таймаут → остаток в `c` по ПЛАНОВОЙ цене (INV-8); `committed` как отдельной сущности нет | U13 | AC-A8 |
| CHK-38 | `agent_band_test::HouseProfitOnFact` | признание прибыли на факте: `realized += fq·(марка−px_факт)/1000·sgn`; позиция агента не трогается (INV-9) | U13 | AC-A8 |
| CHK-39 | `agent_band_test::OwnershipInvariant` | такт клиринг+накопл+перевоз без клиента/исполнения → `Z_a≡0` (INV-1) | U13 | AC-A8 |

**Часть square_bands.py — числовые прогоны (CHK-40..66, 27):**

| ID | GTest-имя | Given → Expected | U | AC |
|---|---|---|---|---|
| CHK-40 | `agent_band_test::Band30_ExternalOrders` | 30 тактов, `Z≠0`: с полосой `31` заявка vs без `80` | U14 | «Эффект полосы» |
| CHK-41 | `agent_band_test::Band30_HedgeCost` | издержки хеджа `182,73` vs `495,06` USD | U14 | «Эффект полосы» |
| CHK-42 | `agent_band_test::Band30_ClientSpread` | спред с клиентов `465,93` в обоих | U14 | «Эффект полосы» |
| CHK-43 | `agent_band_test::Band30_Total` | итог `+203,18` vs `−246,03` | U14 | «Эффект полосы» |
| CHK-44 | `agent_band_test::Band30_CostReduction63pct` | `−63%` издержек (сверка с tol по факту golden) | U14 | «Эффект полосы» |
| CHK-45 | `agent_band_test::TranslatorBandQ18` | переводчик — полоса `q=18` (env `CE_AGENT_BAND_Q_TRANSLATOR`) | U14 | AC-A7 |
| CHK-46 | `agent_band_test::ArbitrageurBandQ30` | арбитражёр — полоса `q=30` (env `CE_AGENT_BAND_Q_ARBITRAGEUR`) | U14 | AC-A7 |
| CHK-47 | `agent_band_test::BandTypesNotMixed` | переводчик не эмитит при `|c|≤18`, даже если превысил условный `30` | U14 | AC-A7 |
| CHK-48 | `agent_band_test::BandHurtsWithoutClientFlow` | `Z≡0`: с полосой итог ХУЖЕ — задокументировано как ожидаемое, НЕ FAIL | U14 | «Эффект полосы» |
| CHK-49..66 | `agent_band_test::TickTrajectory_<k>` (18) | потактовая траектория эталонного прогона: путь позиции `c_j(t)`, тайминг эмиссий, накопление house-прибыли, частичные исполнения/возвраты — **конкретные `def test_*` и допуски подтверждаются при ingestion `square_bands.py`** | U14/U10 | «Эффект полосы»/AC-A6 |

> CHK-49..66 сознательно оставлены блочно: без файла `square_bands.py` их точные имена и
> golden-числа неизвестны. При регистрации golden блок раскрывается 1:1 в именованные кейсы;
> сумма 39 для `square_bands.py` фиксирована (12 в `w_structure` + 27 здесь).

---

## 4. Интеграционные тесты (Kafka + gRPC)

Стиль — как `Testing/f20_*.sh` (полный docker-compose) + in-process тесты по образцу
`cpp/matching/tests/infra/*`, `cpp/ledger/tests/*`.

| # | Сценарий | Транспорт / контракт | Проверка | Этап |
|---|---|---|---|---|
| IT-CE-V2-01 | `ce.clearing.input → ce.position.delta` | Kafka, producer `matching` (`CePositionDeltaBatch`→`AgentDelta`) | consumer `ledger` получает `AgentDelta{agent_id,asset,venue,delta}`; поле `target` **отсутствует** (breaking) | Э2 |
| IT-CE-V2-02 | `ce.position.delta → ledger → GetAgentPositions` | gRPC `LedgerService.GetAgentPositions` (новый) | `ce_agent_position(agent_id,asset,venue)` = накопленная знаковая позиция; реплей той же дельты идемпотентен (guard по `batch_id`) | Э2 |
| IT-CE-V2-03 | Эмиссия по полосе → `execution.intents` | Kafka, producer `risk` | `|c|>q` → ровно один `ExecutionIntent` на `(|c|−q)`; `|c|≤q` → сообщений нет | Э3 |
| IT-CE-V2-04 | `execution.intents → venues → execution.venue` | Kafka round-trip | intent исполняется (полн./частично), `ExecutionReport` в `execution.venue` (+ legacy `execution.reports`, dual-publish CLAUDE.md §7.3) | Э3/Э4 |
| IT-CE-V2-05 | `execution.venue → ledger`: возврат остатка | Kafka consumer в `ledger` | частичное исполнение → остаток в `ce_agent_position` по ПЛАНОВОЙ цене; полное → позиция не двоится (уменьшена на A7) | Э4 |
| IT-CE-V2-06 | Таймаут `in_flight` без `ExecutionReport` | внутренний таймер `ledger` | по TTL весь объём → `c_j`; повторной отправки нет (защита от двойной эмиссии) | Э4 |
| IT-CE-V2-07 | Перевозка арбитражёра | Kafka `ce.transfers` (новый) | `{asset,src,dst,qty,eta,tariff}` публикуется; по `eta≤t` приход на узел; `in_transit` учтён в `владение_a` | Э5 |
| IT-CE-V2-08 | Backward-compat `AgentDelta` vs `AssetDelta` | proto | старый consumer отключён по флагу либо совместимый fallback; тест **фиксирует факт несовместимости**, форсируя миграционный план (CLAUDE.md §11.3) | Э2 |

---

## 5. E2E (`Testing/`, только сценарии — код не пишем)

### `Testing/f18v2_ce_band_e2e.sh` (основной)
1. `docker compose -f infra/docker-compose.dev.yml up --build` c `CE_V2_GRAPH=1 CE_AGENT_POS=1 CE_AGENT_BAND=1 CE_AGENT_A8=1`.
2. N тактов **со** встречным клиентским потоком (`POST /v1/flow-orders` серией, `Z≠0`): число исходящих `execution.intents` существенно меньше числа тактов, где `|c|` превысил бы `q` без полосы (прокси для «−63%»).
3. `GetAgentPositions` (ledger gRPC или SQL к `ce_agent_position`): позиции знаковые, `W·c=0` на срезе.
4. PG: `ce_agent_position` персистентна после рестарта `ledger` (аналог E-CEA-004 F-05A).
5. ClickHouse: рост Kafka HW на `execution.venue`/`ce.position.delta` ⇒ рост строк в CH-таблице.
6. UI (если фронт читает agent-позиции): знаковая позиция по агенту, НЕ `owned−target`.

### `Testing/f18v2_no_client_flow_e2e.sh` (контрольный/негативный)
1. Тот же compose, **без** клиентского потока (`Z≡0`).
2. N тактов только с рыночным дрейфом площадок.
3. Assert `Z_a≡0` для всех активов на всём прогоне (INV-1).
4. Assert: итог ХУЖЕ, чем при `CE_AGENT_BAND=0` — задокументированное ожидание, не регрессия (CHK-48).

---

## 6. Replay-детерминизм (стиль F-15)

| Тест | Input | Expected |
|---|---|---|
| `agent_band_test::ReplayDeterminism` | один `CeClearInput` (снапшот A0) + фикс. конфиг `(α,c,ρ_Q,ρ_T,q_Q,q_T)` дважды | побайтово идентичный `CeClearResult`/`AgentDelta`: тот же `f`, `π`, позиции, набор эмитированных `ExecutionIntent` |
| IT-CE-V2-REPLAY | Kafka replay `ce.clearing.input` (offset reset), тот же `batch_id` | `ledger` не удваивает `ce_agent_position` — идемпотентность обязательна для аудита/бэктеста |
| `w_structure_test::GaugeReplayStable` | реплей с разным начальным seed `π` до фиксации gauge | после `π[USD@Binance]=0` результат не зависит от порядка/seed решателя |

Config-version-аналог F-15 — версия `(α,c,ρ,q)`-конфигурации: та же версия → тот же результат;
смена (например `q_Q: 18→20`) детектируется в диагностике/логах (не тихо).

---

## 7. CI-гейт и калибровка допусков

- 4 новых `.cpp` — самодельный `main()` (`int g_fail; bool close(a,b,tol,what)`), возврат `0`/`1`,
  регистрация в `cpp/matching/CMakeLists.txt` внутри `if(BUILD_TESTING)`.
- Реальный гейт — `make test-ci` (Docker, полный `ctest`). `continue-on-error: true` в
  `.github/workflows/cpp-build.yml` — не гейт (снятие — отдельная задача, не блокирует план).
- Golden-числа U14/CHK-40..66 требуют `ce_tick_v2.py`/`square_bands.py` для калибровки tolerance.
  До ingestion — грубый sanity-check (направление эффекта, не точные цифры); после — ужесточить
  до `1e-6`/`1e-9` по прецеденту F-05A (`ce_agent_clearing_test.cpp`).

## 8. Покрытие A0–A8

| A-раздел | Сценарии | CHK |
|---|---|---|
| A1 три числа/кривая | U1, U2 | 01–07 |
| A2 сдвиг якоря | U3 | 08 |
| A3 матрица W / нумерарий | U4, U5, U7 | 16–24 + 09–10 |
| A4 клиринг | U6, U8 | 11–13 + компл. |
| A5 проверки | U8, U9 | 14–15 |
| A6 накопление | U10 | 28–31 |
| A7 полоса | U11, U12 | 32–36, 45–47 |
| A8 подтверждения | U13 | 37–39 |
| «Позиция биржи» (INV-1) | U13, U14 | 39, 48 |
| «Счёт дома» | U5 | 25–27, 38 |
| «Эффект полосы» | U14 | 40–44, 48–66 |

---

## Отчёт по изменениям

- **Создан:** этот файл (`docs/10-testing/features/F-18-v2-test-plan.md`).
- **Тест-кейсов:** 66 доменных (CHK-01..66) + 8 интеграционных (IT-CE-V2-01..08) + 2 E2E +
  3 replay; 14 сценариев U1–U14; 9 инвариантов INV-1..9.
- **Команды сборки/тестов:** не запускались (docs-only; реализация под гейтом владельца).
- **Отложенные входы (от владельца):** `ce_tick_v2.py` (27) и `square_bands.py` (39) —
  golden-источники; регистрация через skill `ingest-docs` отдельным вызовом (см. §0.1).
- **Риски/следующие шаги:** до ingestion golden — sanity-допуски; после — калибровка 1e-6/1e-9.
  AC→Test перелинковать на реальные AC-ID после `feature.yaml` F-18 v2 (T-F18-005).
