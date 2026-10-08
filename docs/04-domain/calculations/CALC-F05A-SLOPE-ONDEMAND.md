---
id: CALC-F05A-SLOPE-ONDEMAND
title: Кривые ликвидности M1–M4 по глубокому стакану по запросу (панель «Торговля»)
level: L3
feature: F-05A
status: implemented
code:
  - cpp/venues/src/app/liquidity_curve_producer.cpp#ComputeSlopeMethods
  - cpp/venues/src/main.cpp (GET /api/v1/venues/{venue}/slope-curves)
  - frontend/api/server.js#handleVenues (GET /api/venues/slope-curves, GET /api/venues/slope-curves-all)
  - frontend/api/slope_aggregate.js#aggregateLines
tests:
  - cpp/venues/tests/liquidity_curve_producer_test.cpp#TestComputeSlopeMethodsOnDemand
  - cpp/venues/tests/liquidity_curve_producer_test.cpp#TestSlopeMethodsGroundExample
  - frontend/api/slope_aggregate.test.js
ui:
  - route: /main
    component: frontend/web/src/components/RawOrderBook.js
    field: methods[].betaT
sources: [ADR-053, ADR-070, docs/04-domain/business-rules.md#r-f05a-003]
updated_at: 2026-10-07
---

# Кривые ликвидности M1–M4 по запросу

## 1. Смысл

Та же кривая ликвидности биржи, что на вкладке Clearing (способы снятия наклона §6.2),
но по глубокому стакану быстрой полосы (ADR-070) и по сигналу с фронта, а не в цикле по
20 уровням. Показывает, какую линейную цену исполнения обещает биржа на объёме $q$.

## 2. Формула

Для стороны (уровни от лучшего): $D_k=\sum_{i\le k}p_i q_i$, $\delta_k=10^4\,|\ln(\mathrm{VWAP}_k/\mathrm{mid})|$.

$$
\alpha^{M1}=\frac{D_1}{\delta_1},\quad
\alpha^{M2}=\mathrm{OLS}_{D=a+\alpha\delta},\quad
\alpha^{M3}=\frac{D_n^2}{2\sum_k \delta_k \Delta D_k},\quad
\alpha^{M4}=\min_k \frac{D_k}{\delta_k}
$$

$$
\alpha_{ext}=\min(\alpha_{bid},\alpha_{ask}),\qquad
\beta=\frac{\mathrm{mid}^2}{10^4\,\alpha_{ext}},\qquad
P(q)=\mathrm{mid}+\beta\,q
$$

$q>0$ — покупка (ask), $q<0$ — продажа (bid). $\theta=1$ (сырой наклон, сравним с VWAP);
клиринговый $\beta_T=\mathrm{mid}^2/(10^4\,\theta\,\alpha_{ext})$ с $\theta=0{,}6$.

## 3. Числовой пример

§6.2 (mid=60000; $(\delta,D)=(1,100),(3,250),(6,500)$): M1=100, M2≈80,26, M3≈60,98,
M4≈83,33 (`TestSlopeMethodsGroundExample`). Для снимка `MakeSnapshot` (mid=100,5; bid
(100,2),(99,3); ask (101,2),(102,4)) ожидания пересчитываются независимо в
`TestComputeSlopeMethodsOnDemand`.

### Суммарные кривые бирж (панель «Торговля», чекбоксы «По всем биржам»)

Прямая биржи $i$ в осях «цена → объём»: $q_i(p)=(p-\mathrm{mid}_i)/\beta_i=k_i p + c_i$,
$k_i=1/\beta_i$, $c_i=-\mathrm{mid}_i/\beta_i$ (способ $M_m$ один для всех бирж). Объёмы бирж при одной
цене складываются, поэтому сумма линейных функций — линейна (та же формула, что агрегат на вкладке
Clearing, ADR-053):

$$
q_\Sigma(p)=\Big(\sum_i k_i\Big)p+\sum_i c_i,\qquad
q_{\Sigma\setminus s}(p)=\Big(\sum_{i\ne s} k_i\Big)p+\sum_{i\ne s} c_i
$$

$s$ — выбранная биржа. Пример: $\mathrm{mid}_1=100,\beta_1=0{,}5$ и $\mathrm{mid}_2=102,\beta_2=1$ дают
$q_\Sigma=3p-302$ (при $p=101$: $+2-1=+1$) — `slope_aggregate.test.js`. Биржи без свежего глубокого
стакана (аренда не активна) в сумму не входят; ответ перечисляет их в `missing`, UI пишет
«учтено N из M бирж».

## 4. Инварианты и граничные случаи

- $\alpha_{ext}$ по M4 не превышает $D_k/\delta_k$ ни на одном уровне (§6.3-safe); M1–M3 могут переобещать.
- Пустая сторона или нет mid — расчёта нет (404/422, UI: «нет данных»).
- $\alpha\le 0$ или не конечно — $\beta=0$, кривая не рисуется.
- `levels=N` берёт N лучших уровней стороны после сортировки.
- Суммарная кривая зависит от набора учтённых бирж: при частичных данных она занижает сумму ликвидности; поэтому UI всегда показывает «учтено N из M».

## 5. Путь числа

Чекбокс в `RawOrderBook.js` → BFF `GET /api/venues/slope-curves` → `venues`
`GET /api/v1/venues/{venue}/slope-curves` → `ComputeSlopeMethods` по последнему глубокому
снимку быстрой полосы (`VenuesLoop::GetDeepBook`).

## 6. Проверка

`venues_liquidity_curve_producer_test`. Вручную: на тех же уровнях пересчитать $D_k,\delta_k$ и сверить α.

## 7. Открытые вопросы и расхождения

- Клиринг считает по 20 уровням основного снимка, панель — по глубокому; при тонкой
  ближней ликвидности и толстой дальней значения α (особенно M4) могут отличаться
  от клиринговых. Это ожидаемо: снят ограничитель глубины.
