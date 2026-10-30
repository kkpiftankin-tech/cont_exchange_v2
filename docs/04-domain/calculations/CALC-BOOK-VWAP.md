---
id: CALC-F11-BOOK-VWAP
title: Кумулятивный VWAP по уровням стакана биржи
level: L3
feature: F-11
status: implemented
code:
  - frontend/api/book_vwap.js#cumulativeBookVwap
  - frontend/api/server.js#fetchVenueCurveImpl (buildSide)
  - frontend/api/server.js#handleVenues (GET /api/venues/orderbook)
tests:
  - frontend/api/book_vwap.test.js
ui:
  - route: /main
    component: frontend/web/src/components/RawOrderBook.js
    field: bids[].vwap / asks[].vwap
  - route: /vector-clearing-live
    component: frontend/web/src/pages/VectorClearing/VectorClearingLive.js
    field: bidPts[].priceVwap / askPts[].priceVwap
sources: [ADR-053, ADR-070, docs/04-domain/business-rules.md#vwap]
updated_at: 2026-10-07
---

# Кумулятивный VWAP по уровням стакана

## 1. Смысл

Средневзвешенная по объёму цена, по которой можно «съесть» стакан от лучшей цены
до уровня $i$. Показывает реальную цену исполнения рыночного потока заданного объёма
(в отличие от цены самого уровня — маржинальной). Одна и та же формула питает график
кривой на странице VectorClearing (safe-translator, ADR-053) и панель «Торговля»
(график глубины, ADR-070). Только отображение/диагностика: в ledger и клиринг не
попадает, поэтому `Number` (double) допустим (CLAUDE.md §1.1).

## 2. Формула

Уровни упорядочены от лучшего к худшему: ask — по возрастанию цены, bid — по убыванию.

$$
Q_i = \sum_{j \le i} q_j, \qquad
N_i = \sum_{j \le i} p_j \, q_j, \qquad
\mathrm{VWAP}_i = \frac{N_i}{Q_i}
$$

| Обозначение | Смысл | Единицы | Откуда берётся |
|---|---|---|---|
| $p_j$ | цена уровня $j$ | quote | `venue_snapshots.{ask,bid}_depth_json` / `venue_book_live` |
| $q_j$ | объём уровня $j$ | base | то же |
| $Q_i$ | накопленный объём (`cum`) | base | `cumulativeBookVwap` |
| $\mathrm{VWAP}_i$ | `vwap` | quote | `cumulativeBookVwap` |

Знак в системе: покупка (ask) $+$, продажа (bid) $-$ (`q: sign * cumQty` в `buildSide`);
панель «Торговля» рисует объём bid отрицательным (ADR-070).

## 3. Числовой пример

ask-уровни $(p, q)$: $(100, 2)$, $(101, 3)$, $(103, 5)$.

| $i$ | $Q_i$ | $N_i$ | $\mathrm{VWAP}_i$ |
|---|---|---|---|
| 1 | 2 | 200 | 100 |
| 2 | 5 | 503 | 100,6 |
| 3 | 10 | 1018 | 101,8 |

Совпадает с `book_vwap.test.js`.

## 4. Инварианты и граничные случаи

- $\mathrm{VWAP}_i$ лежит между лучшей ценой и ценой уровня $i$; для ask не убывает, для bid не возрастает.
- $\mathrm{VWAP}_1 = p_1$.
- Уровни с нефинитными или неположительными $p$/$q$ пропускаются.
- Порядок накопления фиксирован слева направо: результат побитово совпадает с прежним
  inline-кодом `buildSide` (проверяется тестом на 200 случайных стаканах).

## 5. Путь числа

`venues` (REST depth) → ClickHouse `venue_book_live` / `venue_snapshots` →
BFF `cumulativeBookVwap` → `GET /api/venues/orderbook` (поля `cum`, `vwap` на уровне) →
`RawOrderBook.js` (рисует кривую, сам не считает). Для VectorClearing: `fetchVenueCurveImpl`
→ `buildSide` → `/api/vector-clearing/curve`.

## 6. Проверка

`node frontend/api/book_vwap.test.js`. Вручную: VWAP$_i$ = сумма цена×объём по уровням
до $i$ делить на сумму объёмов.

## 7. Открытые вопросы и расхождения

- Движок клиринга (`cpp/venues`, `depth_curve_builder`) строит кривую ликвидности по
  20 уровням основного снимка, а панель «Торговля» — по глубокому снимку быстрой
  полосы (до 1000 уровней, ADR-070). VWAP одинаков по формуле, но набор уровней разный;
  числа на двух экранах совпадут только на общем диапазоне глубины.
