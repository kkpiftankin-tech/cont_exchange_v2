---
id: DOC-DOMAIN-BUSINESS-RULES
phase: 04-domain
status: draft
owner: core-team
source:
  - IN-005 §6 «Формулы расчётов» (F-12 Execution Hedge)
related:
  - docs/02-system/features/F-12-execution-hedge/
  - docs/04-domain/entities.md
  - docs/04-domain/ubiquitous-language.md
---

# Business Rules — Continuous Exchange / Flow Order Book

Единое место для бизнес-правил и формул расчётов. Каждая формула привязана к фиче и проверяется тестами.

## Финансовая точность

Все суммы денег, объёмов, цен — `fob.common.v1.Decimal` (см. [common.proto](../../contracts/proto/fob/common/v1/common.proto)). `double` допустим только для диагностики (residualNorm, solveTimeMs) и метрик. См. [CLAUDE.md §9](../../CLAUDE.md).

---

## F-04 Batch Clearing

См. [features/F-04-batch-clearing/](../02-system/features/F-04-batch-clearing/) и [matching-fob-core/overview.md](../05-components/matching-fob-core/overview.md). Формулы VWAP и инвариант диапазона цен описаны в [CLAUDE.md §8.5 и §8.4](../../CLAUDE.md).

---

## F-12 Execution Hedge

Источник: IN-005 §6.

### Hedge Trigger

После batch clearing (F-04) для каждого инструмента / провайдера:

$$
\text{triggerNotional} = |\text{netQty}| \times \text{clearingPrice}
$$

$$
\text{trigger} = \big(|\text{netQty}| \geq \text{thresholdQty}[\text{symbol}]\big) \; \text{OR} \; \big(\text{triggerNotional} \geq \text{thresholdNotional}\big)
$$

Если `trigger == true`, формируется `ExecutionIntent` с `target_qty = |netQty|`, `side = sign(netQty)` (BUY если netQty > 0, SELL если < 0).

Параметры (`thresholdQty[symbol]`, `thresholdNotional`) задаются в `hedge_policy_config` (PostgreSQL, планируется); fallback из env.

Реализация: [`cpp/matching/src/app/hedge_trigger_policy.cpp`](../../cpp/matching/src/app/hedge_trigger_policy.cpp).

### targetNotional

$$
\text{targetNotional} = \text{targetQty} \times \text{referenceMid}
$$

`referenceMid` берётся из BatchResult clearing price для текущего symbol (cross-link к F-04).

### Routing Plan

Для набора healthy venues (`venue.health.status = CONNECTED` и `latency_ms <= maxLatency[urgency]`):

$$
\text{qty}[v] = \frac{L(v)}{\sum_{v' \in V_{\text{healthy}}} L(v')} \cdot \text{targetQty}
$$

где $L(v)$ — суммарная ликвидность venue $v$ в нужном направлении (`bid` для SELL, `ask` для BUY), извлекаемая из `SideLiquidityCurve` ([fob.venue.v1.SideLiquidityCurve](../../contracts/proto/fob/venue/v1/venue.proto)) от F-11 топика `venue.liquidity.fob`.

Дополнительные ограничения:
- $\text{qty}[v]$ округляется к `lot_size[venue]` через `floor`.
- $\text{qty}[v] \leq \text{maxOrderSize}[venue]$ (берётся из VenueSnapshot).

Реализация: целевая в Execution Planning (см. [05-components/execution-planning/](../05-components/execution-planning/overview.md), интерфейс [`IExecutionPlanningUseCases`](../../cpp/venues/src/app/execution_planning_uc.hpp)).

### Pre-hedge Risk Check (×3)

Перед отправкой child orders Execution Planning вызывает `RiskService.PreHedgeCheck` (см. [06-api/grpc/risk-pre-hedge-check.md](../06-api/grpc/risk-pre-hedge-check.md)). Три проверки:

1. **Notional limit:** $\quad \text{targetNotional} \leq \text{maxNotionalPerHedge}$
2. **Exposure limit:** $\quad \text{currentHedgeExposure}[\text{symbol}] + \text{targetQty} \leq \text{hedgeExposureLimit}[\text{symbol}]$
3. **Slippage limit:** $\quad \text{expectedSlippage} \leq \text{maxSlippage}[\text{urgency}]$

Если хотя бы одна проверка не прошла — `PreHedgeCheckResponse.ok = false`, HedgeFlow помечается `RISK_REJECTED`, publish `risk.alerts(HEDGE_REJECTED, reason)`.

`maxSlippage[urgency]` по умолчанию:

| urgency | max slippage |
| --- | --- |
| LOW | 5 bps |
| MEDIUM | 15 bps |
| HIGH | 30 bps |

### Child Order Price (с tick rounding)

Базовая цена:

$$
\text{rawPrice} = \text{referenceMid} \pm \delta
$$

где $\delta$ — passive offset (`+\delta` для SELL, `-\delta` для BUY) при `urgency = LOW`; для MEDIUM / HIGH $\delta = 0$ (price = referenceMid или market).

Округление к `tick_size`:

$$
\text{price} = \text{round}\!\left(\frac{\text{rawPrice}}{\text{tickSize}}\right) \times \text{tickSize}
$$

`tickSize` берётся из VenueSnapshot для конкретного `(venue, instrument)`.

### Child Order Quantity (с lot rounding)

$$
\text{childQty} = \min\!\left(\left\lfloor \frac{\text{qty}[v]}{\text{lotSize}} \right\rfloor \times \text{lotSize},\; \text{maxOrderSize}\right)
$$

`lotSize`, `maxOrderSize` — из VenueSnapshot.

### slippageBps

Считается per-fill после получения ExecutionReport:

$$
\text{slippageBps} = \frac{|\text{avgPrice} - \text{referenceMid}|}{\text{referenceMid}} \times 10^4
$$

Реализация: входит в payload `ExecutionReport.slippage_bps` (вычисляется Adapter перед публикацией).

### avgFillPrice (VWAP по fills)

Агрегированная цена по всем fills HedgeFlow:

$$
\text{avgFillPrice} = \frac{\sum_i \text{filledQty}_i \times \text{avgPrice}_i}{\sum_i \text{filledQty}_i}
$$

где $i$ — fills внутри `child_orders`, относящиеся к одному `hedge_flow_id`.

Записывается в `hedgeflows.avg_fill_price`.

### remainingQty

$$
\text{remainingQty} = \text{targetQty} - \text{filledQty}
$$

Где `filledQty = sum(child_orders.filled_qty WHERE hedge_flow_id = X)`.

### Overfill Guard

$$
\text{overfill} = \text{accumulatedFilledQty} - \text{targetQty}
$$

$$
\text{guard\_triggered} = \text{overfill} > \text{overfillThreshold}
$$

`overfillThreshold` по умолчанию: `0.005 * targetQty` (0.5%). При срабатывании Adapter:
1. Отменяет все open child orders (`CancelOrder` через EVC).
2. Публикует `ExecutionReport(status=OVERFILL_GUARD, reason=overfill_race_condition)`.
3. HedgeFlow → terminal (status зависит от политики: COMPLETED или REJECTED).

### Reconciliation Gap

$$
\text{gap} = \text{targetQty} - \text{filledQty}
$$

После истечения `hedgeTimeoutMs` или completion всех child_orders:
- $\text{gap} \leq \text{reconciliationGapThreshold}$ → status=COMPLETED.
- $\text{gap} > \text{reconciliationGapThreshold}$ → если есть fallback venue → retry с `urgency = HIGH`; иначе status=UNDERFILLED + `risk.alerts(HEDGE_UNDERFILL)`.

`reconciliationGapThreshold` по умолчанию: `0.0001 * targetQty` (0.01%, см. F12-N3).

### HedgePnL

> **Conflict Note (2026-09-28).** Карточка вычисления
> [`CALC-F12-HEDGE-PNL`](calculations/CALC-F12-HEDGE-PNL.md) сверила эту
> формулу с реализацией: `calculate_hedge_pnl` в `cpp/ledger/src/app/ledger_uc.cpp`
> **не вычитает** `feesTotal` (это gross hedge PnL); net-метрика с учётом
> комиссий существует отдельно как `summary.netAfterFees` на
> `/hedge-pnl` (SQL-агрегат в `frontend/api/server.js`). Формула ниже и
> вывод "реализация планируется" были неточны на момент правки — код и тест
> (`cpp/ledger/tests/ledger_hedge_pnl_test.cpp`) уже существуют. Не менять
> самостоятельно без `trading-domain-specialist` — см. §7 карточки CALC.

Realized PnL на момент завершения HedgeFlow.

**SELL** (продали на venue, получили quote):

$$
\text{hedgePnL} = (\text{avgFillPrice} - \text{referenceMid}) \times \text{filledQty} - \text{feesTotal}
$$

**BUY** (купили на venue, потратили quote):

$$
\text{hedgePnL} = (\text{referenceMid} - \text{avgFillPrice}) \times \text{filledQty} - \text{feesTotal}
$$

Интерпретация: положительный hedgePnL значит, что биржа исполнила хедж лучше, чем внутренний clearingPrice (зашли с положительным capture).

Реализация (фактическая, gross, без вычитания комиссий): [`cpp/ledger/src/app/ledger_uc.cpp`](../../cpp/ledger/src/app/ledger_uc.cpp)
`calculate_hedge_pnl` + `RecordHedgeExecution` / `apply_execution_report_locked`.
Подробности, числовой пример и путь числа до экрана — в
[`CALC-F12-HEDGE-PNL`](calculations/CALC-F12-HEDGE-PNL.md).

### FillRatio

$$
\text{FillRatio} = \frac{\text{filledQty}}{\text{targetQty}} \times 100\%
$$

Используется в Reconciliation Alerts dashboard.

### GapAbs / GapPct

$$
\text{GapAbs} = \text{targetQty} - \text{filledQty}
$$

$$
\text{GapPct} = \frac{\text{GapAbs}}{\text{targetQty}} \times 100\%
$$

### Post-trade Risk Update

После каждого ExecutionReport (FILLED / PARTIALLY_FILLED) Risk Manager обновляет:

$$
\text{currentHedgeExposure}[\text{symbol}] \mathrel{{-}{=}} \text{filledQty} \times \text{sign}(\text{side})
$$

где `sign(BUY) = +1`, `sign(SELL) = -1`. Снижение absolute exposure при правильном хедже.

Источник: IN-005 §6.

---

## Связь с F-04 / F-11 / F-15

- **F-04 → F-12:** `referenceMid` в ExecutionIntent = `BatchResult.clear_prices[symbol]`.
- **F-11 → F-12:** Routing plan использует `SideLiquidityCurve` из топика `venue.liquidity.fob`.
- **F-12 → F-06:** ExecutionReport обновляет `positions` через Settlement Ledger.
- **F-15:** Backtest должен дать identical HedgePnL при тех же входах через VenueSim.

---

## F-18 — CE band-хедж: трёхзонное правило (Кривые §6.4)

CE-агент (переводчик / арбитражёр) копит знаковую позицию `c_j` (k-USDT) клирингом; наружу
хеджируется по **трёхзонному правилу** от порога `Γ` ([Кривые §6.4](../../incoming-docs/2026-08-06-Кривые_котирования_внутренних_маркет-мейкеров_CE_биржевое_из-e70752d0.md),
[ADR-065](../03-architecture/adr/ADR-065-ce-commission-placement-and-arbitrageur-hedge.md),
[ADR-066](../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md)):

$$
\bar{Z}_{lim} = \frac{c_{lim}}{\Gamma},\qquad \bar{Z}_{mkt} = \frac{c_{mkt}}{\Gamma},\qquad \Gamma = \rho = \gamma\,\sigma^2\,\tau
$$

- **`|c_j| ≤ Z̄lim`** — no-action: наружу ничего, запас смещает якорь (inventory-skew) и помогает
  внутренней разгрузке.
- **`Z̄lim < |c_j| ≤ Z̄mkt`** — **пассивный мейкер-лимит** до цели `Ztarg`: заявка «стоит» несколько
  тактов, накапливает частичное исполнение по мере пересекающих сделок, платит **мейкер-комиссию
  `clim`**; исполнение НЕ гарантировано; при устойчивом неисполнении — эскалация в тейкер **по
  таймауту** (резидентный лимит, [ADR-067](../03-architecture/adr/ADR-067-ce-maker-resting-limit-hedge.md)).
- **`|c_j| > Z̄mkt`** — **агрессивный тейкер-сброс** до цели немедленно (спред + **тейкер-комиссия
  `cmkt`**).

где `clim = ce_maker_fee_bps`, `cmkt = ce_taker_fee_bps`, `Γ` — предельная стоимость риска запаса
(= коэффициент inventory-skew `ρ = γσ²τ`, [ADR-066](../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md)),
`rt` = round-trip (арбитражёр ×2). Учёт позиции при эмиссии/исполнении — модель **A7/A8**
([ADR-061](../03-architecture/adr/ADR-061-ce-v2-agent-position-band-hedge.md)): на эмиссии
`c -= excess`, `in_flight += excess`; fill двигает только `in_flight`; таймаут/EXPIRED возвращает
неисполненный residual в `c`.

Числовой пример (§6.4): `Γ = 0.5‰/тыс`, `clim = 0.25‰`, `cmkt = 0.75‰` ⇒ `Z̄lim = 0.5 тыс`,
`Z̄mkt = 1.5 тыс` USDT. Запас 0.33к после 1 такта — зона ожидания; 0.99к после 3 тактов — лимит
на 0.49к; при упорном разрыве — рыночный сброс по тайм-ауту.

**Динамический порог (2 оси, ADR-066, обе за флагами, обратимы).** Плоский `Z̄=c/Γ` может
масштабироваться: (а) **волатильностная ось** D1 (`CE_BAND_GAMMA_MODE`) — выше `σ` ⇒ у́же band
(`VolScaleFactor`, `σ` из `ce_asset_volatility`); (б) **ликвидностная ось** #7 scoped
(`CE_BAND_LIQ_MODE`, [ADR-066 §D3](../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md))
— глубже стакан ⇒ ШИРЕ band (`LiqScaleFactor=(depth/depth_ref)^{0.5}`, `depth=α_e` из
`ce_asset_liquidity`, writer matching). Композиция `BandTotalScale=clamp(Vol·Liq, tr_min, tr_max)`
(финальный клэмп); обе off ⇒ поведение как до фичи. `Γ=γσ²τ` — калибровка O1 (осознанная MVP,
не тождество эталону IN-017 `A=σ√(2γ/Λ)`; O2 owner-gate) — см. §«F-18 Теоретическая основа (IN-017)».

Источники: Кривые §6.4, CE_algorithm_v2 §A1/§A7/§A8, ADR-061/065/066/067.

### F-18 — Теоретическая основа (IN-017): дилер между двумя биржами

Эталонная модель для якоря и порога band CE-агента — обобщение
Barzykin–Bergault–Guéant (2023) на две сегрегированные непрерывные биржи
([IN-017](../../incoming-docs/2026-09-28-dealer-two-continuous-exchanges-48590e82.md)
§3, §5, §6, §8). Не заменяет действующую реализацию (ADR-061/065/066) —
фиксируется как справочный эталон и источник будущей калибровки (см. Conflict
Note ниже); числовой разбор — карточка
[`CALC-CE-ANCHOR`](calculations/CALC-CE-ANCHOR.md).

**Наклон = ликвидность (Лемма 1).** При линейном спросе биржи
$\Phi_i(p) = \lambda_i(S^i - p)$ и каноническом наклоне дилера
$\kappa_i = \lambda_i$ («зеркало рынка») поток на бирже $i$:
$$
v_i = \frac{\lambda_i}{2}(a^* - S^i).
$$
Выбор наклона не влияет на цену сделки $p_i = S^i + v_i/\lambda_i$ и издержки
$v_i^2/\lambda_i$ — задача сводится к выбору скоростей, а не кривых как таковых.

**Общий якорь (Теорема 1).** Обе биржи котируются от ОДНОГО якоря:
$$
a^* = P - Aq - BD = (1 - w)P + w P_\lambda - Aq,
$$
где $q$ — суммарный запас дилера, $P$ — фундаментальная (мартингальная) цена,
$D = S^2 - S^1$ — разрыв цен бирж, $P_\lambda = (\lambda_1 S^1 + \lambda_2
S^2)/\Lambda$ — цена «слияния» (взвешенная по ликвидности), $\Lambda =
\lambda_1 + \lambda_2$, $r = \sigma\sqrt{\gamma\Lambda/2}$, $A = 2r/\Lambda$,
$\Delta = \lambda_1\phi_1 - \lambda_2\phi_2$, $B = \tfrac{r}{\Lambda}\cdot
\tfrac{\Delta}{r+\rho}$, $w = r/(r+\rho)$. Смысл: якорь — смесь фундаментальной
цены и цены слияния (доля $w$ растёт с риск-неприятием $\gamma$ и падает со
скоростью возврата разрыва $\rho$), минус сдвиг на собственный запас $Aq$.

**Три сущности (Предложение 1).** Тот же якорь реализуется двумя
Переводчиками с карманами $q_1+q_2=q$ и бесплатной внутренней биржей:
$$
p_{\text{int}} = \omega_1 S^1 + \omega_2 S^2 - Aq = a^*,\qquad
\omega_1 = \phi_2 + B,\ \omega_2 = \phi_1 - B,\ \omega_1+\omega_2=1.
$$
**Локальное правило (Следствие 2).** В глубоком пределе разбиения карманов
внешний поток каждого Переводчика зависит только от СВОЕГО кармана:
$$
v_i = -\frac{\lambda_i A}{2\omega_i}\,q_i.
$$

**Пороговое хеджирование (Предложение 2).** При комиссии $c_i$ на внешней
бирже $i$ (внутренняя биржа бесплатна):
$$
\text{Переводчик } i \text{ хеджируется} \iff |q_i| > Q_i :=
\frac{\omega_i c_i}{A},\qquad
v_i = -\frac{\lambda_i A}{2\omega_i}\,\mathrm{sign}(q_i)\,(|q_i| - Q_i)_+
$$
Порог $Q_i$ зависит ТОЛЬКО от собственной позиции кармана — вся кросс-информация
уже свёрнута во внутреннюю цену $p_{\text{int}}$ и карман $q_i$.

> **Conflict Note (IN-017 ↔ CE, 2026-09-29).** Эталонная модель IN-017 и
> действующая реализация CE (ADR-061/065/066) структурно совпадают
> (skew на позицию + пороговое хеджирование), но РАСХОДЯТСЯ в деталях —
> зафиксировано, не выбрано молча:
> 1. **Масштаб порога по риск-неприятию.** CE (ADR-066): `Γ = γσ²τ`, порог
>    `Z̄ = c/Γ` убывает как $1/\gamma$. IN-017 (Предложение 2): `Q_i =
>    ω_i·c_i/A`, `A = σ√(2γ/Λ)` убывает как $1/\sqrt{\gamma}$ — другая
>    степень зависимости от $\gamma$, другая модель (нет аналога `τ`,
>    вместо него — `Λ`, суммарная ликвидность двух бирж).
> 2. **Вес $\omega_i$.** IN-017 взвешивает порог/скорость долей ликвидности
>    и разрыва ($\omega_i = \phi_j \pm B$); текущий inventory-skew CE
>    (`ce_inv_skew_gamma`, `matching_loop.cpp`, ADR-066 §«Соответствие
>    параметрам конфига») — плоский коэффициент без аналога $\omega_i$.
> 3. **Арбитражёр — отдельная сущность.** IN-017 моделирует только два
>    Переводчика + внутреннюю биржу; отдельной сущности «арбитражёр» с
>    иной комиссией/логикой нет. В CE арбитражёр — market-хедж round-trip
>    (ADR-065, признанный MVP-компромисс, альтернатива C «перевозчик» не
>    реализована).
> 4. **Смесь $P$/$P_\lambda$ в якоре.** Общий якорь IN-017 явно содержит
>    $(1-w)P + wP_\lambda$ (смесь фундаментальной цены и цены слияния,
>    сдвиг $BD$ на разрыв). В текущей CE-реализации anchor_eff строится как
>    `anchor − clamp(γ·c_j)` (память `ce-inventory-skew`) без явного
>    вычисления цены слияния/веса `w` — сдвиг на разрыв `B·D` отсутствует
>    как отдельный термин.
>
> Решение зафиксировано: [ADR-066 §Addendum (2026-09-29)](../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md)
> (Accepted) — **владелец выбрал O1**: `Γ=γσ²τ` принят как осознанная MVP-калибровка (не тождество
> с эталоном IN-017); целевой пересмотр к `A=σ√(2γ/Λ)` — при появлении слоя `Λ`/`φ_i` (F-18 #7).
> См. также §7 [`CALC-CE-ANCHOR`](calculations/CALC-CE-ANCHOR.md); текущий код —
> откалиброванная MVP-реализация с явными флагами обратимости
> (`CE_BAND_GAMMA_MODE`). Решение о перекалибровке под IN-017 — за владельцем
> + `trading-domain-specialist`, не самостоятельная правка.

Источники: IN-017 §3 (Лемма 1), §5 (Теорема 1), §6 (Предложение 1, Следствие
2), §8 (Предложение 2). Связанные ADR: [ADR-061](../03-architecture/adr/ADR-061-ce-v2-agent-position-band-hedge.md),
[ADR-065](../03-architecture/adr/ADR-065-ce-commission-placement-and-arbitrageur-hedge.md),
[ADR-066](../03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md).

---

## F-15 — Backtest / Replay

См. [features/F-15-backtest-replay/](../02-system/features/F-15-backtest-replay/).
Полная реализация формул — в [replay_step_journal.cpp](../../cpp/backtest/src/app/replay_step_journal.cpp).

### Базовые рыночные величины (per batch)

$$
\text{mid} = \frac{\text{bestBid} + \text{bestAsk}}{2}
$$

$$
\text{spread} = \text{bestAsk} - \text{bestBid}
$$

`mid` используется как `decisionPrice` для IS. При отсутствии полного
quote — fallback на `clear_price` (см. `decision_price_source` в
[replay_summaries](../07-data/replay-summaries.md)).

### Incremental и cumulative PnL

$$
\Delta PnL_i = PnL^{after}_i - PnL^{before}_i, \qquad
\text{totalPnL} = \sum_i \Delta PnL_i
$$

$$
\text{cumPnL}(t) = \sum_{i \le t} \Delta PnL_i
$$

$PnL^{before}_i$ берётся из `AgentState` предыдущего батча через
[RestoreState UC](../05-components/backtest-service/overview.md).

### Implementation Shortfall

Простая форма:

$$
IS = \text{execPrice} - \text{decisionPrice}
$$

Объёмно-взвешенная (текущий MVP, `avgis_rule = 'volume_weighted'`):

$$
IS_i^{buy} = (P^{avg}_i - \text{decisionPrice}_i) \cdot Q^{exec}_i
$$

$$
IS_i^{sell} = (\text{decisionPrice}_i - P^{avg}_i) \cdot Q^{exec}_i
$$

Aggregated:

$$
\text{avgIS} = \frac{\sum_i IS_i}{\sum_i Q^{exec}_i}
$$

### VWAP

$$
\text{VWAP} = \frac{\sum_j Q^{exec}_j \cdot P^{exec}_j}{\sum_j Q^{exec}_j}
$$

### Fill Rate

$$
\text{FillRate} = \frac{\sum_i Q^{exec}_i}{\sum_i Q^{\max}_i} \times 100\%
$$

Граничный случай: при $\sum_i Q^{\max}_i = 0$ полагается `fill_rate = 0`
и устанавливается флаг `no_requested_volume = true` (F15-34).

### Sharpe ratio (per-batch increments)

$$
\text{Sharpe} = \frac{E[\Delta PnL]}{\sigma(\Delta PnL)}
$$

Граничный случай: при $\sigma(\Delta PnL) = 0$ — `Sharpe = 0` (F15-33;
см. [replay_summary_aggregation_test.cpp](../../cpp/backtest/tests/replay_summary_aggregation_test.cpp)).

### Max Drawdown

$$
\text{MaxDD} = \max_t \big(\,\text{peak}(t) - \text{cumPnL}(t)\,\big),
\qquad
\text{peak}(t) = \max_{s \le t} \text{cumPnL}(s)
$$

### Reward modes

- `pnl` — $r_i = \Delta PnL_i$.
- `-is` — $r_i = -IS_i$ (минимизация IS как награда).
- `hybrid` — $r_i = \alpha \cdot \Delta PnL_i + \beta \cdot (-IS_i)$, веса $\alpha, \beta$ из snapshot.

Default `pnl`. Режим фиксируется в `session_config_snapshot` (F15-26).

### AgentState (state JSON в `replay_agentlogs.state_json`)

Snapshot **до** батча.

```json
{
  "session_id": "rpl-abc-123",
  "batch_seq": 42,
  "event_time_ms": 1739361720000,
  "cum_pnl": 152.30,
  "open_positions": [
    {
      "symbol": "BTCUSDT",
      "qty": 0.05,
      "avg_price": 60100.0,
      "unrealized_pnl": 12.5
    }
  ],
  "free_margin": 9800.50,
  "used_margin": 199.50,
  "mid":    {"BTCUSDT": 60150.0},
  "spread": {"BTCUSDT": 1.2},
  "active_flow_orders_count": 3,
  "session_config_snapshot_version": 1
}
```

### Action (action JSON в `replay_agentlogs.action_json`)

Действие политики на этом батче — какие FlowOrder активны и какие
control-events генерируются. Для MVP стратегия пассивна, FlowOrder
создаются один раз при старте replay'а; `events.kind = "noop"`. RL-агент
в будущем будет генерировать `create_order` / `cancel_order` /
`amend_order` events.

```json
{
  "active_orders": [
    {
      "order_id": "ord-001",
      "symbol": "BTCUSDT",
      "side": "buy",
      "pL": 58000,
      "pH": 62000,
      "qrate": 0.5,
      "qmax_remaining": 95.0
    }
  ],
  "events": [{"kind": "noop"}]
}
```

### ShadowPositions (per-session, in-memory)

Структура `cpp/backtest/src/infra/in_memory_shadow_ledger.cpp` под ключом
`shadow:<session_id>:<account_id>`:

```json
{
  "account_id": "acct-001",
  "namespace": "shadow:rpl-abc-123",
  "balances": {
    "USDT": {"free": 9800.50, "reserved": 199.50},
    "BTC":  {"free": 0.05,    "reserved": 0.00}
  },
  "open_positions": [
    {
      "symbol": "BTCUSDT",
      "qty": 0.05,
      "avg_price": 60100.0,
      "fees_paid": 1.50,
      "cum_realized_pnl": 152.30
    }
  ],
  "shadow_only": true,
  "last_batch_seq": 42
}
```

Инвариант: `shadow_only=true` означает что namespace не имеет влияния на
production Collateral Ledger.

### Инварианты F-15

1. $\text{progress\_batches} \le \text{total\_batches}$.
2. При `solver_error_flag=1` или `risk_status='rejected'`: `fills_applied=0`.
3. $\text{processed\_batches} = N - \text{failed\_batches}$ при completed.
4. $\sum_i Q^{exec}_i \le \sum_i Q^{\max}_i$.
5. Для каждой fill: $\min(p_L) \le P^{exec} \le \max(p_H)$ (наследуется от F-04).
6. Детерминизм: одинаковый `session_config_snapshot` + одинаковый
   `random_seed` + одинаковая входная история → одинаковые
   `replay_agentlogs` (F15-16).
7. `partial=true` ⇔ статус сессии в `{cancelled, failed}` и хотя бы один
   батч обработан.

## F-09 — Batch / Combo / Multi-leg Orders

Источники: IN-011 §2, §9, §10, §11, §20; [ADR-031](../03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md), [ADR-032](../03-architecture/adr/ADR-032-parent-child-order-model.md), [ADR-033](../03-architecture/adr/ADR-033-execution-groups-topic.md). Деньги — `Decimal`; `double` только для `solver_diagnostics`.

### F-09.1 Математика группового исполнения

Вектор исполнений группы \(g\) и отображение в активы:

\[
e_g = (e_{g,1},\dots,e_{g,L_g})^T,\qquad \Delta x_g = B_g\,e_g,\qquad v_g = \frac{B_g\,e_g}{\Delta t}
\]

Ratio/basket — общий масштаб:

\[
e_g = \alpha_g\,\rho_g
\]

Пересчёт цели по notional weight и остаток:

\[
Q^{target}_{g,\ell}(t) = \frac{w_{g,\ell}\,N_g}{P^{ref}_{\ell}(t)},\qquad
Q^{remaining}_{g,\ell} = \max\!\left(Q^{target}_{g,\ell}-Q^{filled}_{g,\ell},\,0\right)
\]

Feasible caps и итоговый масштаб:

\[
Q^{feasible}_{g,\ell} = \min\!\left(Q^{remaining}_{g,\ell},Q^{rate}_{g,\ell},Q^{liq}_{g,\ell},Q^{risk}_{g,\ell},Q^{venue}_{g,\ell}\right)
\]

\[
\alpha_g^{liq} = \min_{\ell}\frac{Q^{feasible}_{g,\ell}}{Q^{target}_{g,\ell}},\qquad
\alpha_g^* = \min\!\left(\alpha_g^{solver},\alpha_g^{liq},\alpha_g^{risk},\alpha_g^{venue},1\right)
\]

Общие линейные ограничения и spread:

\[
A_g\,e_g = b_g\,\alpha_g,\qquad G_g\,e_g \le h_g,\qquad L_g \le c_g^T P \le U_g
\]

Встраивание в клиринг непрерывной биржи (рядом с обычными FlowOrder):

\[
\sum_i v_i + \sum_g \frac{B_g\,e_g}{\Delta t} = 0
\]

\[
\min_{\{v_i\},\{e_g\}}\left[\sum_i L_i(Q_i,v_i)+\sum_g \Phi_g(e_g,\alpha_g)+\sum_g \mathrm{Penalty}_g(e_g)\right]
\]

Группы `orchestration_only` в joint solver не участвуют — каждая нога как независимая FlowOrder.

### F-09.2 Политики исполнения

- **strict_atomic:** все обязательные ноги в пропорции или ничего; \(\alpha_g<\alpha_g^{min}\Rightarrow\alpha_g=0\); orphan legs запрещены; для внешних площадок только при `venue_native` или internal_batch.
- **scalable_atomic:** \(e_g^*=\alpha_g^*\rho_g\); ratio/weights в пределах `max_ratio_deviation_bps`; иначе `waiting_next_batch` (не молчаливая деградация).
- **best_effort:** \(e_{g,\ell}^*=\alpha_g^*\rho_{g,\ell}+\delta_{g,\ell},\;|\delta_{g,\ell}|\le\varepsilon_{g,\ell}\); обязательна фиксация `violatedConstraints`, `fallbackAction`, `ratioDeviation`, `degraded`.
- **sequential_fallback:** только по явному разрешению user/Risk/policy; молчаливый перевод strict/scalable запрещён.
- **external_compensating:** без strict-гарантии; при нарушении структуры — compensating action; статусы `degraded/compensating/rollback_pending/rolledback`; \(\text{external\_compensating}\ne\text{strict\_atomic}\).

### F-09.3 Ключевые инварианты (§20)

1. \(Q^{remaining}_{g,\ell}=\max(Q^{target}-Q^{filled},0)\ge 0\).
2. \(\text{strict\_atomic}\Rightarrow \text{orphanLegs}=0\).
3. \(\text{scalable\_atomic}\Rightarrow e_g=\alpha_g^*\rho_g\) (в пределах tolerance).
4. \(\text{best\_effort}\wedge\exists\,\text{нарушение}\Rightarrow \text{violatedConstraints}\ne\varnothing\).
5. \(\text{external\_compensating}\ne\text{strict\_atomic}\).
6. `ExecutionGroup` фиксируется в `group_state_transitions` до публикации `LegFill`; ledger идемпотентен по `execution_group_id`.
7. \(p^{low}_{\ell}\le \text{exec\_price}\le p^{high}_{\ell}\) (наследие F-04).
8. \(\sum_{\text{batches}}\text{exec\_qty}_{g,\ell}\le Q^{max}_{g,\ell}\).

### F-09.4 Child graph transitions (§11.6)

- **OCO:** исполнение ветви ⇒ siblings → `cancelled`, идемпотентно (`group_state_transitions`).
- **Bracket:** \(Q_{tp}^{max}=Q_{sl}^{max}=Q_{entry}^{filled}\); при частичном entry exits resize инкрементально; TP↔SL взаимная отмена.
- Все переходы идемпотентны по `(executionGroupId, legId)`; зависимости от wall-clock/thread scheduling запрещены (replay determinism, AC-F09-010).

### F-09.5 Таксономия как единая модель

`MultiLegVectorOrder = Legs + Constraints + StateGraph + ExecutionPolicy`. Специализации:

| Тип | Ограничение |
| --- | --- |
| Basket | \(\sum_i w_i=1;\ Q_i^{target}=w_i N/P_i^{ref}\) (строки \(A_g\)) |
| Pair | \(Q_1=kQ_2\) (строка \(A_g\)) |
| Spread | \(L_g\le c_g^T P\le U_g\) (строки \(G_g\)) |
| Factor | \(FQ=0\) (строки \(A_g\)) |
| Budget | \(\sum_i P_iQ_i\le N\) (строка \(G_g\)) |
| Risk | \(R(Q)\le R_{max}\) (строки \(G_g\)) |
| OCO / Bracket | `ConditionalLink` граф (не линейные ограничения) |

Комбинации допустимы (`Basket+Factor+Budget` → несколько строк \(A_g\)/\(G_g\)).

### F-09.6 Связи

F-04 (grouped solver встроен в batch cycle; `batch_id` связывает `ExecutionGroup`↔`BatchResult`); F-06 (leg-level positions/PnL, combined PnL); F-07/F-08 (pre/post-trade grouped checks); F-11 (`venue.liquidity.fob` → \(Q^{liq}\)); F-12 (`external_compensating` через ExecutionIntent/Report); F-15 (replay determinism, AC-F09-010). MVP-рекомендация: decoupled solver (F-04 даёт clearPrices как reference) — см. open question в [implementation-plan/F-09](../implementation-plan/F-09-batch-combo-orders.tasks.md).

## Clearing Mechanics (IN-012)

> Математическая основа matching engine. Canonical reference —
> [`incoming-docs/2026-04-15-continuous-order-market-academic-v1.md`](../../incoming-docs/2026-04-15-continuous-order-market-academic-v1.md)
> (IN-012). См. также
> [entities.md §Continuous-Order Market Primitives](entities.md#continuous-order-market-primitives-in-012)
> и [ADR-035](../03-architecture/adr/ADR-035-fob-solver-mathematical-foundation.md).

### R-CLR-001 Curve Forms Equivalence

**Правило**: пусть `L_i(Q_i, ·)` собственна, замкнута и строго выпукла.
Тогда четыре представления кривой агента эквивалентны:
`V_form ⟺ P_form ⟺ L_form ⟺ H_form` (IN-012 Предложение 3.2).

**Следствие**: solver внутренне работает в `H_form` (гамильтониан → QP),
пользовательский API — `L_form` (`FlowOrder` параметры). Конвертация
валидна тогда и только тогда, когда выполнено strict convexity.

### R-CLR-002 Market Aggregation

Совокупный рынок задаётся канонически (IN-012 Предложение 4.1):

```text
H(Q, P)  = Σ_i H_i(Q_i, P)                          (canonical sum)
L(Q, Q̇) = inf_{Σ_i v_i = Q̇}  Σ_i L_i(Q_i, v_i)   (infimal convolution)
V(Q, P)  = Σ_i V_i(Q_i, P)
```

**Дуальная эквивалентность** (R-CLR-002a):
`Q̇ ∈ ∂_P H(Q, P)  ⟺  P ∈ ∂_Q̇ L(Q, Q̇)`.

### R-CLR-003 Clearing Price Semantics

**Определение**: рыночная цена клиринга `P*(Q)` — **общий множитель
Лагранжа** для ограничения баланса потоков `Σ_i Q̇_i = 0` (IN-012 §4.3).

**Три эквивалентные характеристики**:

1. **Flow conservation**: `Σ_i Q̇_i(Q_i, P*) = 0` (4.2).
2. **Lagrange multiplier**: `P* ∈ ∂_Q̇ L(Q, 0)` (4.7).
3. **Hamiltonian minimum**: `P*(Q) ∈ arg min_P H(Q, P)` (4.15).

Все три используются как corectness invariants в determinism tests F-15.

### R-CLR-004 Solver Preconditions

Достаточные условия существования и единственности клиринга (IN-012
Предложение 4.3):

1. `L_i(Q_i, ·)` собственна, полунепрерывна снизу, **суперлинейна**.
2. `μ_i`-сильно выпукла по скорости, `μ_i > 0`.

Тогда `H(Q, ·)` строго выпукл и коэрцитивен, цена клиринга существует
и единственна.

**Инвариант** (R-CLR-004a): все агенты, попадающие в matching solver,
должны иметь `M_i ≻ 0` (positive-definite inertia matrix). Сингулярные
классы (`InfiniteInertiaAgent`, `PerfectLiquidityAgent`) — предельные
случаи, обрабатываемые регуляризацией (R-CLR-008).

### R-CLR-005 Quadratic Closed-Form

Для класса `L_i(Q_i, Q̇_i) = (1/2) Q̇_i^⊤ M_i Q̇_i + ψ_i(Q_i) − a_i^⊤ Q̇_i`,
`M_i ≻ 0`, цена клиринга в замкнутом виде (IN-012 §4.5):

```text
B := Σ_i M_i^(-1)
b := Σ_i M_i^(-1) a_i

P*(Q) = − B^(-1) b
V(Q, P) = B P + b
H(Q, P) = (1/2) P^⊤ B P + b^⊤ P + const
```

Цена клиринга = взвешенный средний внешний якорь, веса обратно
пропорциональны "инерциям" агентов. Используется как **reference vector**
в unit-тестах solver'а
([continuous_market_replica_test.cpp](../../cpp/matching/tests/domain/)).

### R-CLR-006 N-Agent Aggregation

Для N стандартных одномерных агентов (IN-012 Предложение 6.1):

```text
m* = (Σ_i 1/m_i)^(-1)
a* = m* · Σ_i (a_i / m_i)
p* = a*
```

Вес каждого внешнего якоря **обратно пропорционален индивидуальной
инерции**. Closed-form check в determinism tests.

### R-CLR-007 Multi-Asset Replication

Любой quadratic market `H(P) = (1/2)(P − A)^⊤ Λ (P − A)`, `Λ ≻ 0`,
реализуется `n + r*(Λ)` агентами через `Λ = D + UU^⊤` (IN-012 §6.3.1).
В 2D `r*(Λ) ≤ 1` ⇒ **всегда достаточно 3 агентов**.

**Реализационная импликация**: N-asset solver должен поддерживать
произвольную `Λ ≻ 0`, не делая diagonal assumption. Текущая реализация
`solver_impl.cpp` это делает через sparse `W` matrix.

### R-CLR-008 Singular Regularization

Сингулярные предельные случаи (IN-012 §A) обрабатываются регуляризацией:

- Идеально ликвидный агент (`m → 0`) → добавляем `ε q̇² / 2` к `L_i`.
- Бесконечная инерция (`m → ∞`) → клампируем `M_i ≼ M_max·I`.
- Чистый позиционный штраф без скорости → малый `ε q̇²` для численной
  стабильности.

Текущий `solver_impl.cpp` использует `reg = 1e-12 * diag_max` в normal
equation `W · diag(η) · W^⊤` — согласовано с рекомендацией IN-012 §A.

### Использование Continuous-Order Primitives по features

| Feature | Primitives | IN-012 § |
| --- | --- | --- |
| F-04 (Batch Clearing) | StandardAgent, LinearFeeAgent, Aggregation, Clearing | §4, §5.1, §6.1 |
| F-09 (Combo / Multi-leg) | PortfolioAgent, Multi-Asset Replication | §5.2.8, §6.3.1 |
| F-10 (MM Curves) | PortfolioAgent (factor `w`) | §5.2.8 |
| F-11 (LOB → FOB) | 1D + 2D agent typology для curve calibration | §5.1, §5.2 |

## F-05A — Vectorized External Liquidity

Источник: IN-014. Мат.основа — [ADR-035](../03-architecture/adr/ADR-035-fob-solver-mathematical-foundation.md),
R-CLR (ниже). Политики — [ADR-047](../03-architecture/adr/ADR-047-surplus-exchange-pnl-policy.md) (surplus),
[ADR-048](../03-architecture/adr/ADR-048-qp-solver-backend.md) (QP backend). Деньги — Decimal (§9).

### R-F05A-001 Vector construction

Каждый внешний order level пары `X/Y` с effective price `P_eff` превращается в вектор
активов `w_i ∈ R^N`:

\[
\text{bid: } w_i = e_X - P_{eff}\, e_Y, \qquad \text{ask: } w_i = -e_X + P_{eff}\, e_Y.
\]

`P_eff = P ± fees ± latency_buffer ± slippage_buffer`. Асимметрия знаков bid/ask —
инвариант; нарушение → ошибка векторизации.

### R-F05A-002 No synthetic pair book

Синтетическая книга по парам **не** строится. Matching multi-asset возникает из `Wx=0`,
а не из per-pair книг.

**Гранулярность сегмента (модель, [ADR-051](../03-architecture/adr/ADR-051-f05a-linear-venue-segment.md)):**

- **Модель B (per-level, legacy/сравнение):** каждый внешний level — отдельный
  flow-сегмент и столбец `W`; provenance до `source_order_id`.
- **Модель A (линейная, по умолчанию при `F05A_LINEAR_SEGMENTS_ENABLED`):** ликвидность
  стороны венью = ОДНА линейная функция → **один** сегмент на `(venue, pair, side)` с
  реальным наклоном (`d_hl=|worst−best price|`, `D=|b|`); provenance до
  `(venue, pair, side)`. Синтетическая пара-книга по-прежнему не создаётся (сегмент
  привязан к реальной стороне венью).

В обеих моделях `venue_id`/`pair`/`side` сохранены (source-trace).

### R-F05A-003 Flow segment & demand curve

Сегмент: `(w_i, p_i^L=0, p_i^H=d_i^{HL}, q_i=min(Q_i, rateCap_i), Q_i^{max}=Q_i)`.
Спрос — кусочно-линейная truncated-кривая:

\[
D_i(w_i^\top\pi) = q_i \cdot \mathrm{trunc}\!\left(\frac{d_i^{HL} - w_i^\top\pi}{d_i^{HL}}\right),\quad \mathrm{trunc}\in[0,1],
\]

квадратичная матрица `D = diag(d^{HL}/q)` (SPD). Согласуется с R-CLR-005 (quadratic
closed-form) и R-CLR-001 (curve forms equivalence).

### R-F05A-008 Safe-translator наклон (ADR-053)

Наклон двустороннего сегмента венью считается по **VWAP-глубине с safe-haircut**
(модель «безопасного переводчика на CE»), а не по крайним точкам кривой:

\[
\alpha_{ext}=\min_k \frac{D_k}{|\delta_k|},\quad
D_k=\sum_{i\le k}p_i v_i,\quad
\delta_k=10^4\Bigl|\ln\tfrac{D_k/Q_k}{\text{mid}}\Bigr|,\quad
\alpha_T=\theta\,\alpha_{ext},\ \theta=0.60,
\]

наклон (клиринг в log-ценах, ADR-052/053): `m = mid/(10^4·α_T)`, якорь `a=ln(mid)`;
линейный эквивалент `β_T = mid²/(10^4·α_T) = mid·m` (для отображения). `α_ext` берётся
по **обеим** сторонам (тончайшая ликвидность связывает наклон). Флаг
`F05A_TRANSLATOR_MODEL=log_endpoint` возвращает прежнюю формулу `Δlog(p)/Δq`.
UI показывает `α_ext/α_T/β_T` из сегмента движка (единый источник истины).

**Четыре способа снятия наклона + выбор (ADR-053 §Addendum 2026-09-30, базовый документ
«Кривые котирования» §6.2).** `α_ext` снимается из одной стороны стакана ЧЕТЫРЬМЯ способами
(`D_k`, `δ_k` — как выше; `ΔD_k=D_k−D_{k-1}`, `D_{-1}=0`):

- **M1 касательная:** `α=D_1/δ_1` (точна у mid, оптимистична на объёме);
- **M2 МНК на полосе:** наклон OLS `D=a+α·δ` со свободным членом, `α=[nΣδD−ΣδΣD]/[nΣδ²−(Σδ)²]`
  (наименьшая ошибка объёма — трекает VWAP);
- **M3 подгонка по выгоде:** `α=D_n²/(2·Σ_k δ_k·ΔD_k)` (равенство площадей/денег);
- **M4 минорант (дефолт):** `α=min_k D_k/δ_k` (наибольшая прямая целиком под лестницей).

Проверка на числовом примере §6.2 (mid=60000; `(δ‰,D)=(1,100)(3,250)(6,500)`): M1=100, M2≈80.26,
M4≈83.33, M3≈60.98 — совпадает с печатными числами дока (grounded, unit-тест). Ограничитель
§6.2: `α_T=min{θ·α_ext, W_T/(γσ²τ)}`; `β_T=mid²/(10^4·α_T)`. **Инвариант §6.3 `α_T≤α_ext`** держит
поточечно только M4; M1–M3 могут локально ПЕРЕОБЕЩАТЬ глубину ⇒ безопасны как диагностика/дисплей,
в клиринг — осознанно. Выбранный для клиринга способ — `f05a_clearing_config.slope_method` (1..4,
деф 4=MINORANT; `venues` поллит → `SetClearingSlopeMethod`). Дефолт воспроизводит прежний
`α_ext=min_k D_k/δ_k` байт-в-байт. **Смена способа ≠ MINORANT меняет клиринговые цены** ⇒ owner
sign-off. Наклон снимается всеми 4 способами и кладётся в контракт (диагностика) для отображения;
BFF отдаёт 4 кривые из сегмента движка (не пересчитывает — drift устранён). Инвариант CAL-3
(полка не зависит от наклона) сохранён. Детали и контракт —
[ADR-053 §Addendum](../03-architecture/adr/ADR-053-f05a-safe-translator-liquidity-model.md).

### R-F05A-004 Clearing condition (asset balance)

Клиринг требует баланса активов:

\[
\sum_i x_i w_i = 0 \iff Wx = 0,\qquad 0 \le x \le q.
\]

Это специализация R-CLR-003 (flow conservation `Σ Q̇_i = 0`) на multi-asset векторный
случай. Solver: `max_x [xᵀp^H − ½xᵀDx]` при `Wx=0`, `0≤x≤q` (ADR-048).

### R-F05A-005 Residual & surplus (invariant)

`r = Wx`, `residualNorm = ‖r‖`. Converged ⇔ `‖r‖ < tolerance`. При `‖r‖ > tolerance`
остаток — денежный surplus/deficit, **никогда не скрывается** и обрабатывается по
`surplus_policy` (ADR-047: `REJECT_IF_RESIDUAL` дефолт | `EXCHANGE_PNL` | `SURPLUS_ASSET`
| `MM_LAST_RESORT`). **Инвариант ledger:** несбалансированный `ExecutionGroup` не
применяется — либо `Wx≈0`, либо остаток явно на house-счёт (`Σuser + house = 0`,
no phantom inventory, §17).

### R-F05A-006 Execution mapping & traceability

Для каждого `x_i > 0`: `executed_asset_vector = x_i · w_i`; исполняется исходный
внешний order level. Каждый fill трассируется к `venue_id` / `source_order_id` /
`segment_id` / `batch_id` / `execution_group_id`.

### R-F05A-007 External atomicity

Внешние ноги без native atomic support на venue **не** маркируются `strict_atomic`
(согласуется с F-09 AC-F09-006 / ADR-031).

## F-18 — CE Capital, Net Position & Position-Based Hedge

Источник: [ADR-054](../03-architecture/adr/ADR-054-ce-capital-net-position-hedging.md).

### R-F18-001 Projection of clearing vector onto assets

После **converged** батча вектор клиринга `x` проецируется на чистую позицию по
каждому активу. Для двустороннего сегмента `(venue, BASE/QUOTE)` с знаковым потоком
`x_i` (`x_i>0` — покупка base, `x_i<0` — продажа base) и эффективной ценой
`P_eff = exp(w[quote] − w[base])` (log-price clearing, ADR-052/053):

$$
\Delta pos_{\text{BASE}} \mathrel{+}= x_i,\qquad
\Delta pos_{\text{QUOTE}} \mathrel{-}= x_i\cdot P_{\text{eff}}.
$$

Δposition агрегируется по всем сегментам батча — встречные ноги неттятся. Для
**degraded** батча проекция не применяется (клиринг ненадёжен). Применение
идемпотентно по `batch_id`. Money — только Decimal (§9).

### R-F18-002 Position-based (net) hedge

Хедж эмиттится по **чистой** позиции актива, а не per-segment gross (ADR-049).
При `|pos_a| > θ_a` формируется один хедж на актив с целью flatten к нулю:

$$
\text{side} = \begin{cases}\text{SELL}, & pos_a>0\\ \text{BUY}, & pos_a<0\end{cases},\qquad
\text{qty} = |pos_a|.
$$

Realized PnL/fees исполненного хеджа возвращаются в капитал `ce_capital`
(идемпотентно по `report_id`) и уменьшают `pos_a` на `filled_qty`. Realized PnL
считается против cost-basis по формуле F-12 `calculate_hedge_pnl`
(`(P_fill − b_a)·q·sgn(pos) − fee`). Активируется `CE_NET_HEDGE_ENABLED`; при
выключенном флаге — legacy per-segment путь (ADR-049).

### R-F18-003 Mark-to-market, equity и учётное тождество (канон)

Единая книга — вектор остатков `h_a` (`ce_capital.balance`); позиция выводится
`pos_a = h_a − target_a`. Marks берутся из клиринговых log-цен (ADR-052):
`mark_a = exp(w[a] − w[N])`, где `N` — numeraire (`CE_NUMERAIRE`, default USDT,
`mark_N = 1`, `θ_N = ∞`, **не хеджируется**).

$$
E = \sum_a h_a\cdot mark_a,\qquad uPnL_a = pos_a\cdot(mark_a - b_a).
$$

Инвариант казначейского учёта (с точностью до marks):

$$
E \approx E_{\text{seed}} + \sum_a rPnL_a + \sum_a uPnL_a - \sum fee.
$$

Снапшот `CeTreasurySnapshot.identity_ok` и UI-панель `/ce-capital-live` сверяют
это тождество. `cost_basis` ведётся по WAC (пересчёт при наборе по клиринговой
цене; при сокращении — реализация PnL против него). Это устраняет отсутствие MTM
и неопределённость cost-basis исходной редакции ADR-054.

## Source Fragments

- IN-014 §3, §13, §16 — F-05A vectorization math, w_i/W/D, Wx=0 clearing, surplus
- IN-005 §6 «Формулы расчётов» (все 15 формул F-12)
- IN-005 §1 (термины ExecutionIntent, HedgeFlow, ChildOrder, ExecutionReport, Urgency)
- IN-006 § Канонические сущности и термины, Метрики качества исполнения (F-15)
- IN-011 §2, §9, §10, §11, §20 — F-09 vector solver math, политики, инварианты
- IN-012 §3, §4, §5, §6, §A — Continuous-Order Market clearing mechanics
  (fragments F-03, F-05..F-11, F-18, F-20, F-22 в
  [IN-012.fragment-map.md](../../incoming-docs/IN-012.fragment-map.md))
