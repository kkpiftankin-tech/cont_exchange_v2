---
id: CALC-F18-CE-CLEARING-DIAG
title: Диагностика решения CE-клиринга — отклонение ребра, полка, запас и порог типового цикла (панель «Решение клиринга» на вкладке «Граф»)
level: L3
feature: F-18
status: implemented
code:
  - cpp/matching/src/domain/ce_clearing_snapshot.hpp#BuildCeClearingSnapshotJson
  - cpp/matching/src/app/matching_loop.cpp#on_ce_clearing_input (сохранение снимка), MatchingLoop::last_ce_clearing_snapshot_json
  - cpp/matching/src/main.cpp (GET /ce/clearing/last)
  - cpp/market_data/src/domain/agent_builder.hpp#BuildQuoteAgent (dz_age / dz_spread / dz_commission)
  - cpp/market_data/src/app/market_data_uc.cpp#BuildAndPublishCeClearingInput (dropped)
  - frontend/api/ce_clearing_view.js#buildClearingView
  - frontend/api/server.js (GET /api/vector-clearing/ce-clearing)
tests:
  - cpp/matching/tests/domain/ce_clearing_snapshot_test.cpp
  - cpp/market_data/tests/agent_builder_test.cpp
  - frontend/api/ce_clearing_view.test.js
  - frontend/web/src/pages/CeGraph/__tests__/CeClearingPanel.smoke.test.js
ui:
  - route: /ce-graph-live
    component: frontend/web/src/pages/CeGraph/CeClearingPanel.js
    field: solve.edges[], solve.cycles[], solve.nodes[], explanation, liquidity.venues[]
sources: [ADR-079, ADR-055, ADR-064, ADR-069, ADR-071, ADR-075, docs/04-domain/calculations/CALC-CE-GRAPH.md]
updated_at: 2026-10-16
---

# Диагностика решения CE-клиринга

## 1. Смысл

Показывает не оценку, а **настоящее решение** последнего такта CE-клиринга и объясняет, почему поток по ребру или между биржами есть или его нет. Опирается на два числа: отклонение цены ребра от решения $|d|$ против его полки $c$ и, для межбиржевого перемещения, разрыв цен цикла $|G|$ против суммы полок цикла $\Sigma c$. Ничего не считает на фронте и не попадает в ledger (`double` допустим, CLAUDE.md §1.1).

## 2. Формулы

Ребро (переводчик или арбитражёр), потенциалы узлов $\pi$ в промилле:

$$
d=\sigma^*-(\pi_{u}-\pi_{v}),\qquad f=\alpha\cdot\mathrm{sign}(d)\cdot(|d|-c)_+,\qquad \text{запас}=c-|d|
$$

$\sigma^*$ — якорь ребра (у арбитражёра — после сдвига по позиции, ADR-071), $\alpha$ — глубина (тыс. USDT на ‰), $c$ — полка. Состояние: `active`, если $|d|>c$ и $\alpha>0$; `dead_zone`, если $|d|\le c$; `no_depth`, если $\alpha\le 0$.

Полка переводчика складывается из трёх частей, арбитражёра — из издержек перевода:

$$
c_T=c_{\text{ком}}+c_{\text{спред}}+c_{\text{возраст}},\qquad c_{\text{возраст}}=k\,\sigma\sqrt{\text{возраст}}\ \ (\text{ADR-075}),\qquad c_A=\mathrm{CE\_TRANSFER\_C}_{\text{актив}}
$$

Типовой цикл (актив $a$, биржи $v_i<v_j$): переводчик $a@v_i$, перевод $a@v_i\to a@v_j$, переводчик $a@v_j$, перевод $U@v_i\to U@v_j$ ($U$ — нумерарий USDT). Потенциалы при обходе цикла сокращаются:

$$
G=\sigma_{T_i}-\sigma_{T_j}-\sigma_A+\sigma_U,\qquad \Sigma c=c_{T_i}+c_{T_j}+c_A+c_U,\qquad \text{запас цикла}=\Sigma c-|G|
$$

Необходимое условие потока по циклу: $|G|>\Sigma c$ (иначе хотя бы на одном ребре $|d|\le c$). Запас цикла положителен ⇒ цикл `blocked`, значение — сколько ‰ не хватает; отрицателен ⇒ `open`.

| Обозначение | Смысл | Единицы | Откуда берётся |
|---|---|---|---|
| $\sigma^*$ | якорь ребра | ‰ | `edges[].anchor_pm` (до сдвига — `anchor_raw_pm`, сдвиг — `skew_pm`) |
| $\Delta\pi$ | разность потенциалов концов | ‰ | `edges[].potential_gap_pm` |
| $d$ | отклонение | ‰ | `edges[].deviation_pm` |
| $c$ | полка | ‰ | `edges[].dead_zone_pm`, состав `dz_age_pm`, `dz_spread_pm`, `dz_commission_pm` |
| $\alpha$ | глубина ребра | тыс. USDT/‰ | `edges[].depth` |
| $f$ | поток | тыс. USDT | `edges[].flow_kusdt` |
| $G$ | разрыв цен цикла | ‰ | `cycles[].drive_pm` |
| $\Sigma c$ | порог цикла | ‰ | `cycles[].threshold_pm` |

1 ‰ = 10 б.п.

## 3. Числовой пример

Тест `ce_clearing_snapshot_test.cpp`, случай A: якоря $\sigma_{T_i}-\sigma_{T_j}-\sigma_A+\sigma_U=1{,}0$ ‰, полки четырёх рёбер в сумме 1,2 ‰ ⇒ $|G|=1{,}0<\Sigma c=1{,}2$, запас 0,2 ‰, цикл `blocked`, поток 0, потенциалы узлов 0. Случай B: $G=2{,}0$ при $\Sigma c=1{,}2$ ⇒ `open`, реальный поток решателя согласован с формулой $f$. Случай C: $|G|=\Sigma c$ ⇒ `blocked` (граница: поток нулевой).

## 4. Инварианты и граничные случаи

- $G$ не зависит от решения (только якоря), порог — только от полок; запас цикла воспроизводим вручную из таблицы рёбер.
- Условие $|G|>\Sigma c$ необходимое, не достаточное: балансы по другим рёбрам могут не пустить поток; реальный поток всегда берётся из решения, а не из $G$.
- Цикл открыт, только если $\Sigma c-|G|<-10^{-9}$ ‰ (допуск на округление double) и у всех четырёх рёбер глубина $\alpha>0$; иначе `blocked` или `no_depth`. Ребро с нефинитным $d$ получает состояние `invalid`.
- Снимок хранит последний такт: если `market_data` не публикует `ce.clearing.input` (все котировки устарели или отброшены), он остаётся прежним. BFF считает решение старше 30 с устаревшим и не пишет «потока нет» (`explanation.level=error`).
- Число рёбер между тактами колеблется (26–37 на dev): фильтр тонких бирж (`CE_MIN_DEPTH_RATIO`) отсекает пары, глубина которых меньше доли глубины лучшей биржи этой пары; отброшенные видны в `dropped`.
- Цикл строится, только если в графе есть все четыре ребра (иначе пропуск): отброшенная котировка (`stale`, `thin_depth`, `invalid_depth`) убирает цикл из списка, причина видна в `dropped`.
- Нет снимка (`matching` ещё не считал или недоступен) ⇒ `solve = null`, в ответе причина, таблицы решения не рисуются.
- Покрыты только циклы «актив × пара бирж через USDT»; кросс-пары (ETH/BTC, SOL/BTC) присутствуют как рёбра и потенциалы, отдельных циклов-треугольников нет (ADR-079, «Последствия»).

## 5. Путь числа

панель «Решение клиринга и ликвидность бирж» (`/ce-graph-live`) → `GET /api/vector-clearing/ce-clearing` (BFF, `ce_clearing_view.js`) → `GET /ce/clearing/last` (`matching`) → `BuildCeClearingSnapshotJson` ← граф и результат `ClearCe` из `on_ce_clearing_input` ← Kafka `ce.clearing.input` (`CeClearingInput`, `CeQuoteAgent.dz_*`, `dropped`) ← `market_data` (`BuildQuoteAgent`, `BuildAndPublishCeClearingInput`). Состояние бирж — `GET /api/v1/venues` (`venues`, админ-API).

## 6. Проверка

Тесты из `tests`. Вручную: в панели взять строку цикла, сложить полки четырёх рёбер из таблицы рёбер и сравнить с порогом; $G$ — по якорям рёбер: $\sigma_{T_i}-\sigma_{T_j}-\sigma_A+\sigma_U$.

## 7. Открытые вопросы и расхождения

- Полка переводчика на dev состоит почти целиком из члена по возрасту данных, который зависит от завышенной оценки волатильности (память `ce-volatility-merged-stream-bias`): замена оценки — решение владельца.
- Циклы-треугольники через кросс-пары не показаны (ограничение ADR-079).
- Снимок хранит только последний такт, истории решений нет.
