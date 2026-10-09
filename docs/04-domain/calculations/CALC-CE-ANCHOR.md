---
id: CALC-CE-ANCHOR
title: Общий якорь дилера a* и порог хеджа Q_i (эталонная модель IN-017)
level: L3
feature: F-18
status: partial # структура совпадает с CE (skew + порог), коэффициенты и вес ω_i расходятся — см. §7
code:
  - cpp/matching/src/app/matching_loop.cpp (anchor_eff = anchor − clamp(γ·c_j), inventory-skew; НЕ реализует A/B/w из теоремы 1 — см. §7)
  - cpp/ledger/src/app/ledger_uc.cpp#detect_and_emit_band_breach_locked (трёхзонный порог Z̄lim/Z̄mkt; НЕ реализует Q_i=ω_i c_i/A — см. §7)
  - cpp/ledger/src/app/ledger_uc.cpp#LoadBandFeeConfig
tests: [] # прямого unit-теста эталонной формулы нет — это research/reference карточка, не код-путь
ui: [] # величина не выведена на фронт напрямую; см. Z̄lim/Z̄mkt в /ce-agents (Γ-порог)
sources:
  - incoming-docs/2026-09-28-dealer-two-continuous-exchanges-48590e82.md#5 (Теорема 1)
  - incoming-docs/2026-09-28-dealer-two-continuous-exchanges-48590e82.md#8 (Предложение 2)
  - docs/04-domain/business-rules.md#f-18--теоретическая-основа-in-017-дилер-между-двумя-биржами
  - docs/03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md
  - docs/03-architecture/adr/ADR-065-ce-commission-placement-and-arbitrageur-hedge.md
  - docs/03-architecture/adr/ADR-061-ce-v2-agent-position-band-hedge.md
updated_at: 2026-09-29
---

# Общий якорь a* и порог хеджа Q_i (эталонная модель IN-017)

## 1. Смысл

Теоретический эталон для двух практических величин CE: (1) куда сдвигается
котировочный якорь под влиянием запаса и разрыва цен между двумя площадками;
(2) при каком запасе Переводчик обязан хеджироваться наружу, а не
интернализовать поток бесплатно через внутреннюю биржу. Это reference-модель
(IN-017, обобщение Barzykin–Bergault–Guéant), НЕ прямое описание текущего
кода — см. Conflict Note в business-rules.md и §7 ниже.

## 2. Формула

$$
a^* = P - Aq - BD = (1-w)P + wP_\lambda - Aq,\qquad
|q_i| > Q_i := \frac{\omega_i c_i}{A}\ \Rightarrow\ \text{хедж наружу}
$$

| Обозначение | Смысл | Единицы | Откуда берётся |
|---|---|---|---|
| $a^*$ | общий якорь дилера (обе биржи) | цена | Теорема 1(i) |
| $P$ | фундаментальная (мартингальная) цена | цена | Определение 1 |
| $P_\lambda$ | цена «слияния» бирж, $(\lambda_1S^1+\lambda_2S^2)/\Lambda$ | цена | Определение 1 |
| $q$ | суммарный запас дилера, $q=q_1+q_2$ | шт | состояние |
| $D$ | разрыв цен, $S^2-S^1$ | цена | состояние |
| $A$ | сдвиг якоря на единицу запаса, $2r/\Lambda$, $r=\sigma\sqrt{\gamma\Lambda/2}$ | цена/шт | Теорема 1 |
| $B$ | сдвиг якоря на разрыв, $\tfrac{r}{\Lambda}\tfrac{\Delta}{r+\rho}$ | безразм. | Теорема 1 |
| $w$ | вес цены слияния в смеси, $r/(r+\rho)$ | безразм. | Теорема 1 |
| $\omega_i$ | вес биржи $i$ в якоре, $\omega_1=\phi_2+B$, $\omega_2=\phi_1-B$ | безразм. | §6 |
| $Q_i$ | порог позиции Переводчика $i$ для внешнего хеджа | шт | Предложение 2 |
| $c_i$ | комиссия внешней биржи $i$ | цена/шт | §7/§8 |
| $\gamma,\sigma,\rho$ | риск-неприятие, вола фундам. цены, скорость возврата разрыва | — (research) | §4.1 |

Все величины модели — `double`/research (диагностика теоретической модели, не
денежный путь ledger); при переносе в CE-конфиг денежные параметры (`c_i`,
пороги в k-USDT) обязаны быть `Decimal` (CLAUDE.md §9).

## 3. Числовой пример (из IN-017, базовый набор §5)

$\sigma=1{,}00,\ \rho=2{,}00,\ \lambda_1=8{,}00,\ \lambda_2=2{,}00,\
\phi_1=0{,}300,\ \phi_2=0{,}700,\ \gamma=0{,}400$ →
$r=1{,}41,\ A=0{,}283,\ B=0{,}0414,\ w=0{,}41,\ \omega_1=0{,}741,\
\omega_2=0{,}259$.

При комиссиях $c_1=c_2=0{,}2$ (§8):
$$
Q_1 = \frac{0{,}741 \times 0{,}2}{0{,}283} = 0{,}524,\qquad
Q_2 = \frac{0{,}259 \times 0{,}2}{0{,}283} = 0{,}183.
$$
Внутри порога — интернализация через внутреннюю биржу (бесплатно); излишек
$|q_i|-Q_i$ хеджируется с интенсивностью $\lambda_i A/(2\omega_i)$.

## 4. Инварианты и граничные случаи

- $A>0$ всегда (при $\gamma,\Lambda>0$) — якорь всегда сжимается к нулю запаса.
- При $\Delta=0$ (конформный случай, §5 ступень 4): $B=0$, запас стационарно
  нулевой, $a^*=P=P_\lambda$.
- При $\lambda_2\to0$ (Следствие 1) — вырождается в однобиржевую модель BBG
  без хедж-пула.
- $Q_i \ge 0$ всегда ($\omega_i, c_i, A \ge 0$ в допустимой области параметров);
  при $c_i=0$ (бесплатная внешняя биржа) $Q_i=0$ — хедж немедленный (сводится
  к беспороговому Следствию 2).

## 5. Путь числа

Формула НЕ инкорпорирована напрямую в код CE — это reference-модель.
Практический путь аналогичной величины в текущей системе: matching
`anchor_eff` (inventory-skew, `matching_loop.cpp`, `ce_inv_skew_gamma`) → CE
клиринг → ledger `detect_and_emit_band_breach_locked` (порог
`Z̄lim=c_lim/Γ`, `Γ=γσ²τ` по ADR-066) → `hedgeflows`/BFF → фронт `/ce-agents`.
См. §7 для расхождений формул на каждом шаге.

**Ликвидностная ось порога — реализована scoped (ADR-066 §D3, за флагом `CE_BAND_LIQ_MODE`):**
`LiqScaleFactor(depth,depth_ref,0.5,…)`=`(depth/depth_ref)^0.5` и композиция
`BandTotalScale=clamp(VolScaleFactor·LiqScale, tr_min, tr_max)` в `ledger_uc.cpp`;
сигнал `depth`=α_e: matching `UpsertBatch`→PG `ce_asset_liquidity`→ledger `LoadAssetDepth`.
Это ЧАСТИЧНАЯ реализация O2 (`√`-зависимость по `Q∝√Λ`), сигнал — внутренний прокси α_e,
не внешняя `Λ`. Unit-тест `ce_band_liq_scale_test`.

## 6. Проверка

Формула воспроизводится вручную по §3 (совпадает с числами статьи, теория ↔
прототип `prototype_dealer.py`: $\hat c=0{,}917\pm0{,}010$ против теории
$0{,}919$ — IN-017 §11). Прямого unit-теста ЭТОЙ формулы в репозитории нет
(research/reference карточка). Косвенно связанные CE-тесты (проверяют
ДЕЙСТВУЮЩУЮ реализацию, не формулы IN-017):

- `cpp/ledger/tests/ce_band_hedge_a7_test.cpp` (модель учёта позиции A7:
  эмиссия/fill/timeout вокруг порога band).
- `cpp/ledger/tests/ce_band_vol_scale_test.cpp` (масштабирование
  `Γ=γσ²τ` по волатильности, ADR-066 D1).

## 7. Открытые вопросы и расхождения

1. **Степень зависимости порога от $\gamma$.** IN-017: $Q_i \propto
   \omega_i c_i/A$, $A=\sigma\sqrt{2\gamma/\Lambda}$ → порог $\propto
   1/\sqrt{\gamma}$. CE (ADR-066): $Z̄=c/\Gamma$, $\Gamma=\gamma\sigma^2\tau$
   → порог $\propto 1/\gamma$. Разные модели, не сведены; см. Conflict Note
   в business-rules.md.
2. **Вес $\omega_i$ отсутствует в CE.** Текущий inventory-skew
   (`ce_inv_skew_gamma`) — плоский коэффициент на все Переводчики/пары, без
   аналога $\omega_i=\phi_j\pm B$ (доля ликвидности/разрыва конкретной пары
   бирж).
3. **Арбитражёр — две ноги (ADR-069, 2026-10-05, Accepted).** Прежде CE сворачивал
   перевоз в ОДНУ позицию на каноническом dst-узле с round-trip-порогом `rt=2` —
   это теряло ногу src и противоречило IN-017 §6 (per-venue карманы `q_i`) и
   «Кривые» §5 (src/dst — разные узлы). Теперь единый агент `A_*` держит ДВЕ ноги
   (`Δ=−f` на src, `Δ=+f` на dst), по одной заявке на биржу (BUY src / SELL dst),
   порог `rt=1` на ногу (одна taker-комиссия площадки). Вес ноги `ω_i=λ_i/Λ`
   (доля ликвидности площадки в сети актива) — реализован за флагом
   `CE_BAND_OMEGA_MODE` (`LoadAssetDepthTotal`), MVP-заглушка истинных `ω_i=φ_j±B`
   (§2): лидерство цены `φ_i` не наблюдаемо. **Не эквивалентно IN-017 §6:** у
   CE-арбитражёра НЕТ внутренней бесплатной биржи с `p_int=a*` (TRANSFER-ребро
   `anchor=0`) ⇒ пороги `Q_i` локально корректны (та же математика Предложения 2),
   но без доказанной оптимальности теоремы. Полная модель (внутренний обмен +
   `φ_i`-оценщик) — открытый §A8.1 ADR-065.
4. **Явный якорь-смесь $(1-w)P+wP_\lambda$ не реализован.** CE считает
   `anchor_eff = anchor − clamp(γ·c_j)` без явного вычисления $P_\lambda$,
   $w$, сдвига $BD$ на разрыв между площадками.
5. **Решение (владелец, 2026-09-29): O1** —
   [ADR-066 §Addendum](../../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md)
   (Accepted): `Γ=γσ²τ` принят как осознанная MVP-калибровка (не тождество с `A=σ√(2γ/Λ)`
   статьи). Расхождения выше — зафиксированный выбор, не баг/дрейф. Целевой пересмотр к
   `A`/`ω_i` (O2) — при появлении слоя оценки `Λ`/`φ_i` (связать с F-18 #7 «ликвидностная ось»),
   новым решением владельца. Кода O1 не трогает.
6. **Inventory-skew на ноги арбитражёра (ADR-071, 2026-10-07, Accepted).** Обратная
   связь позиция→цена `a_eff = a − clamp(γ·b)` распространена с переводчиков `T_*` на
   TRANSFER-рёбра `A_*` (у них `a=0` ⇒ `a_eff=−clamp(γ_A·b_A)`). База `b = c+in_flight =
   I−F` (накоплено клирингом − подтверждённые fill'ы) = нетто «внутренняя − подтверждённая
   внешняя» (A7, ADR-061): отправка хеджа базу не меняет, снижают только подтверждённые
   fill'ы ⇒ при неподтверждённом хедже позиция растёт и ограничивается ТОЛЬКО сдвигом
   якоря (решение владельца). Отдельные `γ_A/K_A` (`f05a_clearing_config.ce_arb_inv_skew_*`).
   Знак проверен тестом (накопление на приёмнике `j` снижает поток `i→j`). Код:
   `matching_loop.cpp` (база agent|venue, передача в ассемблер) + `ce_graph_assembler.hpp`
   `AssembleCeGraphV2` (сдвиг якоря transfer-ребра). Тест
   `cpp/matching/tests/domain/ce_graph_assembler_test.cpp`.
7. **Лимит хеджа отвязан от сдвинутого якоря (ADR-072, 2026-10-08, Accepted).** Ранее
   лимит хеджа = `price_used` = клиринговая цена СО skew (`anchor_eff`), из-за чего при
   длинной позиции переводчик продавал ниже справедливой цены (убыток). Теперь matching
   публикует fair-цену `F_0` (= `P_λ`, depth-взвешенное среднее цен бирж ДО skew) в
   `AssetDelta.fair_price`, а ledger квотирует лимит `L=F_0 ± m·(1−urg)`, `m=F_0·bps`,
   `urg=min(1,|c|/Z̄mkt)` — маржа прибыли в maker-зоне, слив по `F_0` в taker-зоне;
   инвариант `L_sell≥F_0, L_buy≤F_0` (сдвиг остаётся, но в границах прибыли). Skew
   (`ce_inv_skew_gamma`) теперь действует только на CE-канал (виртуальная цена). Realized/
   unrealized P&L переводчика — [CALC-CE-TRANSLATOR-PNL](CALC-CE-TRANSLATOR-PNL.md). Код:
   `matching_loop.cpp` (fair_price), `ledger_uc.cpp#detect_and_emit_band_breach_locked` (клэмп).
8. **Единый источник порога (наведение порядка, 2026-10-09, фаза 1).** Порог `Z̄lim/Z̄mkt`
   считается **только** в ledger `detect_and_emit_band_breach_locked` (per-currency: σ по
   активу `LoadAssetSigma`, depth по активу/venue, vol/liq/omega-масштаб ADR-066) и
   сохраняется в `AgentPositionState.z_lim/z_mkt` → отдаётся в `GetAgentPositions.z_lim/z_mkt`
   (proto 13/14). BFF/фронт **только отображают** эти значения, НЕ пересчитывают (ранее BFF
   считал свой ПЛОСКИЙ порог `kBand·fee` + отдельный vol-preview → дисплей≠реальность; баг
   усугублялся падением config-SELECT на отсутствующей колонке `gamma`, см. миграцию init.sql).
9. **Конфиг порога в одном месте (наведение порядка, 2026-10-09, фаза 2).** Все ключевые
   ручки порога — в `f05a_clearing_config`: `ce_band_fee_k` (k_band), `ce_maker/taker_fee_bps`
   (clim/cmkt), `gamma`, + новые `ce_band_q_floor`, `ce_band_gamma_mode`, `ce_band_sigma_ref`,
   `ce_band_vol_ratio_min/max`. ledger `LoadBandFeeConfig` читает их из PG (TTL-кэш ~1с), env —
   только fallback. Два отдельных SELECT (core-4 и новые-5) в своих try/catch — отсутствие
   новых колонок не роняет чтение core-4 (регрессия класса бага `gamma`). **Caveat деплоя:**
   PG-значения АВТОРИТЕТНЫ и перебивают env (в т.ч. PG-дефолты на свежем томе перекроют
   `CE_BAND_GAMMA_MODE`/`SIGMA_REF` env — переносить env→PG при миграции); применять `ALTER`
   из init.sql на существующих томах ДО выката ledger. Остаточно (display-only): BFF vol-preview
   ещё читает `sigma_ref` из env — разойдётся с PG после правки (actual-порог из ledger, не затронут).
