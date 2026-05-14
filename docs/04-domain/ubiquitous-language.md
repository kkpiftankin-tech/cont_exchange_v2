---
id: DOC-DOMAIN-UBIQUITOUS-LANGUAGE
phase: 04-domain
status: draft
owner: core-team
related:
  - docs/01-business/glossary.md
  - docs/04-domain/entities.md
  - contracts/proto/fob/common/v1/common.proto
---

# Ubiquitous Language

Технические доменные термины, общие для команды и кода. Бизнес-термины — в [01-business/glossary.md](../01-business/glossary.md).

## Continuous Scaled Limit Orders (CSLO)

- **CSLO** — параметрическая кривая «цена-скорость» / «цена-объём», размещаемая участником. В FOB-модели заменяет дискретный limit order.
- **Kyle-Lee parametrization** — «купить до Q единиц по ценам [P_L, P_H] с максимальной скоростью U».
- **Параметры CSLO:** P_L (нижняя цена), P_H (верхняя), Q (макс. объём), U (макс. скорость, tokens/sec).
- **Demand/Supply curve** — функция v(p) сопоставляющая цене скорость покупки/продажи.
- **Residual curve** — кривая одного трейдера после вычитания заявок конкурентов.

## Flow trading и FOB

- **Flow Trading** — концепция торгов потоками во времени с непрерывной скоростью v_t, а не дискретными ордерами.
- **Flow Order Book (FOB)** — аналог limit order book в пространстве потоков; хранит FlowOrders и решает batch-clearing.
- **FlowOrder** — потоковая заявка: `orderId`, `portfolioWeights`, `p_L`, `p_H`, `q_rate`, `q_max`, `time_in_force`, `status`. Proto: [`fob.orders.v1.FlowOrder`](../../contracts/proto/fob/orders/v1/orders.proto).
- **LOB-to-FOB translation** — преобразование внешнего LOB в FOB-эквивалент.
- **Fundamental value (Pₜ)** — теоретическая «справедливая» цена; моделируется броуновским движением или affine jump-diffusion.

## Цены и воздействие

- **Price impact** — изменение цены под действием торговли участника.
- **Temporary impact** — пропорционально скорости v_t, исчезает после завершения сделки.
- **Permanent impact** — устойчивое смещение цены, связанное с изменением inventory MM и информационной утечкой; зависит от объёма.
- **Average execution price** — средняя цена покупки/продажи объёма x с учётом всей траектории v_t и P_t.
- **Utility / CARA risk aversion** — экспоненциальная функция полезности от −IS с постоянной абсолютной риск-аверсии.
- **Certainty Equivalent IS** — детерминированный эквивалент стохастического IS при оптимальной скорости.

## Matching

- **Batch** — один цикл клиринга в FOB.
- **BatchResult** — proto [`fob.matching.v1.BatchResult`](../../contracts/proto/fob/matching/v1/batch.proto): `clearPrices`, `executedRates`, `residualNorm`, `solveTimeMs`, `solverDiagnostics`, `fills`.
- **clearPrices** — равновесные цены клиринга по каждому инструменту в данном batch.
- **executedRates** — итоговые скорости исполнения по каждому FlowOrder.
- **residualNorm** — мера несоответствия спроса и предложения после решения; меньше — лучше.
- **solveTimeMs** — длительность работы solver на batch.
- **solverConfig** — параметры из PostgreSQL: `batchIntervalMs`, `maxIterations`, `epsilonLiquidity`, `tolerance`, `feeModel`, `isActive`.
- **Fill / FillEvent** — proto-нога BatchResult: `fillId`, `orderId`, `assetLegs`, `execQty`, `execPrice`, `liquiditySource`, `fees`, `timestamp`.

## Risk и ledger

- **Ledger / Settlement Ledger** — логическая книга учёта (free / reserved / venue-allocated / pending) по каждой паре (user, asset).
- **Reservation** — резерв средств под открытый ордер; идемпотентно по `reservation_id`.
- **RiskSnapshot** — снимок маржи: free_collateral, reserved_collateral, initial_margin, maintenance_margin, risk_flags.
- **AgentLog** — state-action-reward запись для агентов (matching, risk, collateral, execution).
- **Kill switch** — глобальный или per-instrument флаг приостановки торгов.

## Decimal

- **Decimal** ([proto](../../contracts/proto/fob/common/v1/common.proto), [cpp](../../cpp/common/include/cex/common/decimal.hpp)) — фиксированная точка `units * 10^(-scale)`. Никогда не используется `double` для денег.

## События и топики

| Топик Kafka | Producer | Сообщение |
|---|---|---|
| `marketdata.raw` | venues | `fob.marketdata.v1.MarketDataRaw` |
| `orders.normalized` | order-flow | `fob.orders.v1.OrdersNormalized` |
| `batch.outputs` | matching | `fob.matching.v1.BatchResult` |
| `execution.intents` | matching (планируется) | `fob.execution.v1.ExecutionIntent` |
| `execution.reports` | venues | `fob.execution.v1.ExecutionReport` |
| `risk.alerts` | risk | `fob.risk.v1.RiskAlert` |

См. также [docs/06-api/messaging/topics.md](../06-api/messaging/topics.md).
