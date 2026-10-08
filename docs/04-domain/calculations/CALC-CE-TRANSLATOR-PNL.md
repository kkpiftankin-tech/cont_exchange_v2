---
id: CALC-CE-TRANSLATOR-PNL
title: Прибыль переводчика — realized (закрытые round-trip) и unrealized (MTM остатка)
level: L3
feature: F-18
status: proposed # формула определена в ADR-072 §C; код пока считает только агрегат R+U (tradedBasketUsdt)
code:
  - frontend/api/trade_stats_math.js#tradedBasketUsdt (сейчас только агрегат R+U; раздельные R/U — цель ADR-072)
  - frontend/api/server.js#fetchAgentTradeStats (источник buy/sell qty, avg_price из venue_fill_diagnostics)
  - frontend/api/server.js#buildAgentRows (pnl_usdt)
tests: [] # цель — frontend/api/trade_stats_math.test.js (тождество C2) + ledger unit на кумулятивный R
ui:
  - /vector-clearing (разбор агента — строка заработка; цель: R и U раздельно)
sources:
  - docs/03-architecture/adr/ADR-072-ce-translator-profit-hedge-price-skew-resting.md#c-realized--unrealized-pl-переводчика
  - docs/04-domain/calculations/CALC-CE-ANCHOR.md
  - docs/04-domain/calculations/CALC-F12-HEDGE-PNL.md
  - incoming-docs/2026-09-28-dealer-two-continuous-exchanges-48590e82.md
updated_at: 2026-10-08
---

# Прибыль переводчика: realized / unrealized P&L

## 1. Смысл

Реальный заработок переводчика — на **внешней бирже** (на CE — виртуальное
движение для набора/разгрузки позиции). Прибыль = поймать отклонение цены своей
биржи от справедливой (клиринговой) цены $F_0$ «между биржами»: исполнить хедж не
хуже $F_0$, когда рынок биржи отклоняется в выгодную сторону. Метрика должна
**накапливать** пойманный спред (realized), отдельно от переоценки открытого
остатка (unrealized) — иначе заработок маскируется MTM и «стоит на месте».

## 2. Формула

Для (agent, venue, symbol) по знаковым хедж-филлам (`venue_fill_diagnostics`,
`hedge_flow_id LIKE 'ce|band|%'`) и CE-дельтам (ledger):

- $B,\ \bar p_B$ — суммарный объём и средняя цена ПОКУПОК;
- $S,\ \bar p_S$ — суммарный объём и средняя цена ПРОДАЖ;
- $n = B - S$ — открытый остаток; $m_{match} = \min(B, S)$ — закрытый объём.

$$R = m_{match}\,(\bar p_S - \bar p_B), \qquad U = n\,(F_0 - \bar p_{open}), \qquad \bar p_{open} = \begin{cases} \bar p_B & n > 0 \\ \bar p_S & n < 0 \end{cases}$$

Агрегат (прежняя `tradedBasketUsdt`):

$$R + U = S\bar p_S - B\bar p_B + n F_0 \quad(\text{касса} + \text{марка остатка})$$

| Обозначение | Смысл | Единицы | Откуда |
|---|---|---|---|
| $R$ | realized — прибыль на закрытых round-trip | quote (→USDT) | закрытый объём $m_{match}$ |
| $U$ | unrealized — MTM открытого остатка | quote (→USDT) | $n$ по $F_0$ |
| $F_0$ | справедливая цена ДО skew | цена пары | matching (ADR-072 §A) |
| $\bar p_B,\bar p_S$ | ср. цена покупок/продаж | цена пары | `avg_price` VFD |
| $B,S$ | объём покупок/продаж | база | `filled_qty` VFD |

Денежный путь (settlement/ledger) — только `Decimal`; `double` допустим для
дисплея BFF (CLAUDE.md §1). Перевод quote→USDT — по $F_{quote}$ на момент закрытия
(как в [CALC-CE-ANCHOR](CALC-CE-ANCHOR.md)).

## 3. Числовой пример

CE-покупка 3 по 84800, хедж-продажа 2 по 84850, $F_0=84840$:

- $m_{match}=\min(3,2)=2$, $R = 2\cdot(84850-84800) = 100$.
- $n = 3-2 = 1$, $\bar p_{open}=\bar p_B=84800$, $U = 1\cdot(84840-84800) = 40$.
- Итого $R+U = 140$. Проверка: касса $= 2\cdot84850 - 3\cdot84800 = 169700 - 254400 = -84700$; $+\,n F_0 = 84840$ ⇒ $140$. ✓

## 4. Инварианты

- **C1:** $R$ меняется ТОЛЬКО при закрытии объёма (рост $m_{match}$); переоценка $F_0$ его не двигает.
- **C2:** $R + U = \text{cash} + n F_0$ (тест-тождество).
- **C3:** $R$ — нарастающий; при $\gamma=0$ и лимите $L\ge F_0$ (ADR-072 §A, инвариант A1) каждый закрытый round-trip даёт $R\ge0$ от хедж-ноги.
- $R=0$ при $B=0$ или $S=0$ (нет закрытого объёма — только unrealized).

## 5. Путь числа

`venue_fill_diagnostics` (buy/sell `filled_qty`, `avg_price`) + ledger CE-дельты →
BFF `fetchAgentTradeStats`/`buildAgentRows` → $R,U$ → `/vector-clearing` (разбор
агента). Сейчас код считает только агрегат $R+U$ (`tradedBasketUsdt`), который
болтается у нуля; раздельные $R/U$ — цель ADR-072 §C.

## 6. Открытые вопросы

1. **$F_0$ ещё не публикуется matching'ом** (ADR-072 §A, поле контракта) — до этого
   $U$ считать по текущей марке пары (приближение), $R$ корректен без $F_0$.
2. **Источник CE-ноги для round-trip.** Reализованный $R$ считается по хедж-филлам
   (реальные деньги); нужно ли включать виртуальные CE-дельты в $B/S$ — решается
   вместе с §A (CE виртуально, реальный P&L — внешние сделки). Провизорно $R$ — по
   хедж-филлам внешней биржи.
3. **Выбор $m$ и $F_0$** — см. ADR-072 §A (на утверждение владельца).
