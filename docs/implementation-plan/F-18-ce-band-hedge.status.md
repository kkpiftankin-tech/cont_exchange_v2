# F-18 CE Virtual Counterparties — статус band-хеджа, комиссии и учёта позиции

> Написано для: владельца проекта и команды — сводка сделанного и оставшегося по
> контуру CE-агентов (переводчики/арбитражёры), band-хеджу и приведению к базовым
> докам. Дата: 2026-09-24. Ветка: `feat/F-18-ce-treasury-nop`.
> Источники истины: [CE_algorithm_v2](../../incoming-docs/2026-09-15-CE_algorithm_v2.md) §A1–A8,
> [Кривые котирования ММ CE](../../incoming-docs/2026-08-06-Кривые_котирования_внутренних_маркет-мейкеров_CE_биржевое_из-e70752d0.md) §6.4.

## 1. Модель (напоминание)

CE — виртуальные контрагенты двух типов: **переводчики** (маркет-мейкер на одной
площадке, пара base/quote) и **арбитражёры** (перевозят актив между площадками,
пара asset/USDT). Позиция `c_j` (k-USDT, знаковая) стартует с нуля, копится клирингом
(`c ← c + f`), и пока `|c_j| ≤ порог` — наружу ничего не идёт. Прибыль — на счёт дома.

## 2. Сделано (реализовано и проверено на dev)

### 2.1 Живость и наблюдаемость цепочки
- **Оживление зависшей цепочки**: после reboot/suspend venues CPU-spin, ledger отдаёт
  stale; `up -d --force-recreate` (не plain restart). Память: `ce-pipeline-freeze-force-recreate`.
- **Liveness вкладки Clearing**: `dataAgeMs` = server clock − newest batch event_time_ms →
  live/stale-баннер + 5-стадийная полоса (стаканы→кривые→клиринг→позиции→хеджи).
- **Единообразные пары X/Y** для всех агентов; хедж-заявки в паре валют.
- **Кнопка «Сбросить позиции»** — реальный сброс c_j/in_flight в ledger (ResetAgentPositions RPC).
- **Живая панель позиций агентов** + drill-down по агенту (модалка, тёмная палитра).

### 2.2 Кросс-пары и корректность объёмов
- **Tick-collapse фикс**: грубый venue-wide tick_size=0.01 схлопывал глубину кросс-пар
  (ETH/BTC, SOL/BTC) в canonicalizer → guard по относительной цене. `AgentPosition.base_price`
  добавлен для корректных ног в валютах пары. Память: `ce-cross-pair-tick-collapse`.
- **Дренаж кросс-пар**: band-fill уменьшал c_j ценой ПАРЫ, а не P_base USDT → позиции
  кросс-пар не сливались; фикс через `st.base_price`. Память: `ledger-band-fill-crosspair-value`.

### 2.3 Скорость торговли и симуляция исполнения
- **Speed-scale**: CE-путь использовал q_grid (сток), теряя скорость v=q/τ кривой;
  масштаб α·(dt/τ) в market_data (`CE_SPEED_SCALE`). Память: `ce-speed-scale`.
- **Staleness-окно ленты**: окно матчинга = N × ИЗМЕРЕННЫЙ период чтения (~40с, rate-limited),
  не MD-loop 5с. Память: `venues-sim-trade-staleness`.
- **Price-impact** в симуляторе: `p_exec = S + k·v`, `cost = k·v²·Δt` (temporary impact),
  `CE_PRICE_IMPACT_K`. Отражается в drill-down.
- **Лесенка = множество матчинга**: drill-down показывает то же свежее окно recent_trades,
  что потребляет матчер (не 60-по-счёту); инвариант «пересекающие сделки ⇔ fill ⇔ дренаж».
  Память: `ce-sim-ladder-equals-matcher`.

### 2.4 Inventory-skew (обратная связь позиция→цена)
- `anchor_eff = anchor − clamp(γ·c_j, ±clamp)`; γ/clamp live-tunable из `f05a_clearing_config`
  (`ce_inv_skew_gamma`/`ce_inv_skew_max_pm`). γ=0.005/clamp=8 стабильно. Память: `ce-inventory-skew`.

### 2.5 Арбитражёры хеджируются
- Убран translator-only gate в ledger; арбитражёры пробивают band и хеджируются в
  asset/USDT. Память: `ce-arbitrageur-band-hedge`.

### 2.6 Дрейф позиции — три дефекта исполнения хеджа (устранены)
Память: `ce-band-hedge-throughput-drift`.
- **A**: хедж не исполнялся (MARKET/IOC заявка выставлялась пассивным лимитом над рынком →
  none_cross_limit). Фикс: venues honors `EXEC_STRATEGY_MARKET` → кроссит всю свежую ленту.
- **B**: утечка in_flight (неисполненный IOC репортился NEW, не терминал → резерв не
  освобождался). Фикс: IOC при неполном исполнении → EXPIRED (терминал) → release.
- **Throughput**: консьюмер `venues_exec` в бэклоге 8400 из-за ~294 падающих PG-операций/мин
  (стабильный `hedge_flow_id` ломал `child_orders_idem`/`hedgeflows_filled_le_target`). Фикс:
  уникальный `client_order_id = intent_id`; hedgeflow `ON CONFLICT` аккумулирует target.
  Результат: PG-ошибки 294→0, lag 8400→~25, binance-интенты 0→норма.

### 2.7 Порог band привязан к комиссии внешних бирж + трёхзонная модель (Кривые §6.4)
Память: `ce-band-threshold-from-commission`.
- Две границы: `Z̄lim = k_band·clim·rt` (no-action → пассив-мейкер), `Z̄mkt = k_band·cmkt·rt`
  (пассив → агрессив-тейкер). rt=round-trip (арбитражёр ×2 — две биржи). clim=`ce_maker_fee_bps`,
  cmkt=`ce_taker_fee_bps`, k_band=`ce_band_fee_k` (=1/Γ), floor `CE_AGENT_BAND_Q_MIN`. Live из PG.
- risk выбирает `EXEC_STRATEGY_MARKET` (taker) vs `EXEC_STRATEGY_LIMIT` (maker) по `AgentBandBreach.aggressive`.
- Проверено: обе зоны эмитятся, дифференциация FILLED/EXPIRED, live-перенастройка (fee=2→q≈5/7.2, fee=26→46.8/93.6).

### 2.8 Учёт позиции по модели A7 (сходимость к band)
Память: `ce-band-position-accounting-a7`.
- Было: c уменьшалась только по исполнению + in_flight-гард → заявка = маржинальный поток →
  позиция ПАРКОВАЛАСЬ (T_BTC ~314), не сходилась.
- Стало (док §A7/§A8): на эмиссии `c -= excess` (c→±q), `in_flight += excess`; fill двигает
  только in_flight; таймаут/EXPIRED возвращает `residual` в c. Заявка = весь избыток → крупная →
  сходится. Проверено: c+in_flight 314→~5 (у band).
- Наблюдаемость: фронт показывает **обязательство = c_j + in_flight** (истинная позиция);
  BFF `c_total`; фильтры по c_total.

## 3. Надо сделать (follow-ups по сверке с доками)

| # | Задача | Приоритет | Источник |
|---|---|---|---|
| 1 | ✅ **СДЕЛАНО (2026-09-25)** inventory-skew на полное обязательство `c + in_flight`: matching `FetchAgentPositionsCached` суммирует position+in_flight; BFF agent-detail skew/band выровнены на c_total + трёхзонные пороги; фронт показывает обязательство и зоны maker/taker | Высокий | §A2 + A7 |
| 1a | ✅ **СДЕЛАНО (2026-09-25)** Арбитражёры переноса номинала (A_USDT_*, asset==numeraire) больше не паркуются: `detect_and_emit_band_breach_locked` сметает их план-позицию к band в счёт дома (без рыночного хеджа «USDT/USDT», владение не трогаем — §«клиринг/накопление/перевозы владение не меняют»). Порог = z_lim от комиссии (rt=2). Лог «F-18 numeraire swept to house». Проверено: A_USDT держится у band 7.2 при активном потоке (было ~1253). Остаток follow-up: явное кредитование house-баланса нумерарием (сейчас дренаж плана, ownership по доку не меняется) | Высокий | §A3/§«Счёт дома», numeraire freedom |
| 2 | **k_band = 1/Γ через `gamma`**: сейчас `ce_band_fee_k` независим; §6.4 задаёт `Z̄=c/Γ`, Γ уже есть как `gamma` в конфиге — связать | Средний | Кривые §6.4 |
| 3 | **Развести комиссию полки vs порога**: §A1 кладёт комиссию в полку кривой, §6.4 — в пороги; проверить, что не задваивается (полка=подавление потока за такт; band=частота хеджа) | Средний | §A1 vs §6.4 (конфликт доков) |
| 4 | **Порог арбитражёра от транспортного тарифа**, а не бирж. taker-fee: у арбитражёра «рыночного риска нет», издержка = тариф перевоза (0.20‰/0.05‰) + амортизация партии | Средний | §A7 |
| 5 | **Симуляция мейкер-исполнения**: пассивный лимит сейчас fill'ится только если свежая сделка пересекла (single-shot IOC); §6.4 подразумевает резервирование до цели с мейкер-комиссией на fill | Средний | Кривые §6.4 |
| 6 | **Мейкер/тейкер комиссия на fill**: сейчас fill несёт price-impact, но не различает clim/cmkt в PnL хеджа | Низкий | Кривые §6.4 |
| 7 | **Динамический band по активу/волатильности/ликвидности**, не только комиссия | Низкий | Память `ce-dynamic-band-threshold` |
| 8 | **Признание прибыли по факту расчёта** (в прототипе разрыв план/факт) | Низкий | §A8, «Незакрытое» #1 |
| 9 | ✅ **ЧАСТИЧНО (2026-09-25)** unit-тест `cpp/ledger/tests/ce_band_hedge_a7_test.cpp` (+ CTest): A7-эмиссия (c→band, избыток в in_flight), A7-fill (c не трогаем), A7-terminal с ЧАСТИЧНЫМ исполнением (возврат остатка), трёхзонные пороги, numeraire-sweep — зелёные. Остаток: throughput-инвариант (0 падающих PG-операций) — интеграционный (live PG/Kafka), отдельно | Высокий | §20 CLAUDE.md |
| 10 | **Выбор q при V>2 площадок** (полос столько же, сколько агентов) | Низкий | §A7, «Незакрытое» #3 |

## 4. Инфраструктура / деплой
- dev `nik@ubuntu-dev`; C++ билды ~20-30 мин (BUILD_JOBS=1); frontend webpack ~3-4 мин;
  BFF = docker cp server.js + restart. После B→A7 нужен один reset-positions.
- Конфиг live-настройки в `f05a_clearing_config`: `ce_taker_fee_bps`, `ce_maker_fee_bps`,
  `ce_band_fee_k`, `ce_inv_skew_gamma`, `ce_inv_skew_max_pm`.
