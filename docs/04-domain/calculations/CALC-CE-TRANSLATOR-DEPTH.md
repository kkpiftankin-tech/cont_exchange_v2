---
id: CALC-CE-TRANSLATOR-DEPTH
title: Итоговая глубина переводчика CE — вес по возрасту, множитель исполнения и капитальный предел
level: fish
feature: F-18
status: implemented (за флагами, по умолчанию выключено; 2026-10-26)
code:
  - cpp/market_data/src/domain/depth_trust_scale.hpp#AgeTrustWeight
  - cpp/market_data/src/domain/depth_trust_scale.hpp#CapitalDepthCap
  - cpp/market_data/src/domain/depth_trust_scale.hpp#ParseCapitalByKey
  - cpp/market_data/src/domain/depth_trust_scale.hpp#ResolveCapitalW
  - cpp/market_data/src/domain/fill_depth_scale.hpp#ResolveFillDepthScale
  - cpp/market_data/src/domain/agent_builder.hpp#BuildQuoteAgent
  - cpp/market_data/src/app/market_data_uc.cpp (сборка котировок агентов, второй проход)
tests:
  - cpp/market_data/tests/depth_trust_scale_test.cpp
  - cpp/market_data/tests/fill_depth_scale_test.cpp
  - cpp/market_data/tests/agent_builder_test.cpp
  - cpp/market_data/tests/ce_input_guard_test.cpp
ui:
  - /ce-graph-live — панель «Решение клиринга», список отброшенных котировок (причина «устарели»)
sources:
  - docs/03-architecture/adr/ADR-085-ce-translator-depth-trust-and-capital-cap.md
  - docs/03-architecture/adr/ADR-082-venue-liquidity-measurement-and-feedback.md
  - docs/03-architecture/adr/ADR-053-f05a-safe-translator-liquidity-model.md
  - incoming-docs/2026-08-06-Кривые_котирования_внутренних_маркет-мейкеров_CE_биржевое_из-e70752d0.md (§2.3, ур. 6.2)
updated_at: 2026-10-26
---

# CALC-CE-TRANSLATOR-DEPTH — итоговая глубина переводчика

## 1. Смысл

Глубина $\alpha$ — наклон кривой переводчика: за такт он исполняет $w=\alpha(\sigma^*-\sigma)$ тыс. USDT на отклонение курса $\sigma$ от якоря $\sigma^*$. Единицы: тыс. USDT на ‰ за такт. Чем она больше, тем больше потока идёт через площадку. Ниже — как складывается итоговая глубина, от наклона внешней книги до последнего множителя.

## 2. Формула

$$
\alpha_T=w\cdot f\cdot\min\{\theta\,\alpha_{ext}\cdot s,\ cap\}
$$

| Символ | Что это | Откуда |
| --- | --- | --- |
| $\alpha_{ext}$ | наклон внешней книги: минорант (M4, по умолчанию) или регрессия по окну объёма (M5) | `agent_builder.hpp`, ADR-077 |
| $\theta$ | доля безопасной трансляции, на dev 0,5 | `F05A_SAFE_SHARE` |
| $s=dt/\tau_{кривой}$ | скорость-масштаб: «запас» книги превращается в «поток за такт» | `CE_SPEED_SCALE` |
| $cap=W_T/(\gamma\varsigma^2\tau)$ | капитальный предел, тыс. USDT на ‰; $W_T$ — капитал именно этого переводчика (по ключу «биржа\|пара», запасное значение для остальных) | ADR-085 (раздел 4), ур. (6.2) базового документа |
| $f$ | множитель исполнения биржи (ADR-082 шаг 1), от $f_{min}=0{,}1$ до 1 | `CE_DEPTH_FILL_COEF` |
| $w$ | вес по возрасту снимка (ADR-085), от 0 до 1 | `CE_TRUST_MODE` |

Вес $w$: сырой $\psi=\max(0,1-\Delta t/T_{stale})$ (LINEAR) либо $\psi=1/(1+\Delta t/T_\varsigma)$, $T_\varsigma=(\sigma_0/\varsigma)^2$ (DISP); итоговый $w=\max\bigl(0,(\psi-\psi_{min})/(1-\psi_{min})\bigr)$. При $w=0$ котировка исключается из клиринга (причина `stale`).

Порядок в коде: тонкие котировки отсекаются по сырой глубине; затем предел $cap$ (только котировки к номиналу), затем множители $f$ и $w$.

## 3. Числовые примеры

Страница «Интерактивный клиринг CE v6», патчи 1–2, и пример базового документа (проверены тестом `depth_trust_scale_test`).

| Случай | Входные данные | Результат |
| --- | --- | --- |
| LINEAR | возраст 190 мс, $T_{stale}=500$ мс | $\psi=0{,}620$; глубина 50 ⇒ 31,0 |
| DISP | возраст 190 мс, $\sigma_0=0{,}5$‰, $\varsigma=0{,}79$‰/√с ⇒ $T_\varsigma=0{,}4006$ с | $\psi=0{,}678$; глубина 50 ⇒ 33,9 |
| DISP, проверка специалиста | $\sigma_0=0{,}3$‰, $\varsigma=0{,}09$‰/√с ⇒ $T_\varsigma=11{,}1$ с; возраст 5 с | $\psi=0{,}69$; при $\varsigma\times 2$ ⇒ 0,36 |
| порог $\psi_{min}=0{,}05$ | $\psi=0{,}62$ | $w=(0{,}62-0{,}05)/0{,}95=0{,}6$; при $\psi\le 0{,}05$ ⇒ $w=0$, исключение |
| предел | $W_T=10$, $\gamma\varsigma^2=0{,}4$ ‰/с ($\gamma=0{,}1$, $\varsigma=2$), $\tau=0{,}5$ с | риск на такт 0,2‰, $cap=50$; при $\tau=1$ с — 25 |
| базовый документ, ур. (6.2) | $\theta\alpha_{ext}=0{,}6\cdot 83{,}3=50$ | предел 60 ⇒ 50; предел 40 ⇒ 40 |
| порядок | $\theta\alpha=50$, $w=0{,}69$, $f=0{,}9$, $cap=25$ | $0{,}621\cdot 25=15{,}5$ (предел первым); «предел последним» дал бы 25 без действия $w$ и $f$ |

## 4. Инварианты

- $0\le w\le 1$, $\alpha_T\le\min\{\theta\alpha_{ext}s,\ cap\}$ (предел сохраняется, потому что $w,f\le 1$).
- Глубина строго убывает с возрастом (при выключенном пределе и при связывающем).
- Нет данных (нет возраста, волатильности площадки, неположительный вход) ⇒ множитель 1 и предела нет: глубину не режем.
- Нулевая глубина в клиринг не передаётся: котировка с $w\le 10^{-9}$ или с итоговой глубиной ниже $10^{-8}$ исключается и попадает в диагностику отброшенных (`stale`, `invalid_depth`); актив без ссылок ни в одной роли (базовой и котируемой валюты кросс-пары) не передаётся, а вход проверяется перед публикацией (`FindInconsistentQuote`), иначе `matching` падает на `node_index.at`. Порядок «предел, затем $f$ и $w$» — в `ComposeDepth`, порог возраста — в `TrustDropThresholdMs`; обе функции в тесте.

## 5. Открытое

- $\varsigma$ из `ce_asset_volatility` завышена в 1,5–2,9 раза и растёт с частотой опроса: DISP и предел зависят от неё (рекомендуется LINEAR, пока оценка не заменена).
- Предел возраста LINEAR по умолчанию константа (60 с); адаптивный предел по σ площадки (`CE_TRUST_TSTALE_ADAPTIVE=1`) на dev упирается в 10 с и режет глубину обычных котировок на 40–90 % (ADR-085, «Пробное включение на dev»).
- Предел $cap$ действует на тактовую глубину после скорость-масштаба (как в базовом документе); альтернатива — на «запас» до масштаба с горизонтом $\tau_{hold}$ — отличается множителем $s$ и поглощается калибровкой $W_T$, $\gamma$ (ADR-085, Conflict Note 2).
- Значения $W_T$ и $\gamma$ не заданы; ориентир — тактовые глубины стенда (ADR-085, «Данные стенда»). Пробы на dev (ADR-085, «Пробы капитального предела») дали калибровку: при $\tau=1$ с и $\varsigma\approx0{,}1$ ‰/√с (ETH, SOL; у BTC около 0,06) предел $\approx100\,W_T/\gamma$; $W_T=1000$ и $\gamma=60$ режут тело глубины самой ликвидной биржи (`T_BTC_binance`: медиана 6 439 ⇒ 1 389), $\gamma\approx20$ — только крайний хвост. $W_T$ задаётся по ключу «биржа\|пара» (ADR-085, раздел 4); откуда брать значения (сейчас настройка, позже, возможно, таблица с экраном) — решение владельца.
- Шаг 3 ADR-082 (порог не выше цели плюс заявка) ждёт замера шагов 1–2; формула $H^*=\sqrt{2c_{fix}v_{in}/\gamma\varsigma^2}$ со страницы v6 отклонена (ADR-082, шаг 2).
