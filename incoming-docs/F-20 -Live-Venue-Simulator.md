# F-20. Симулятор исполнения на внешних площадках (Live Venue Simulator)

---

[TOC]

## 1. Обзор

### 1.0. Понятия и определения (F-20 Live Venue Simulator)

#### F-20 Live Venue Simulator

**Определение**
Фича, реализующая гибридный режим работы системы: приём **живых** дискретных стаканов (LOB) от внешних CEX/DEX/AMM в боевом режиме через F-11-инфраструктуру с одновременной **симуляцией** исполнения child-ордеров от непрерывной биржи без реального выхода на рынок. Симулятор использует текущий живой VenueSnapshot как источник ликвидности для расчёта реалистичного fill-ответа, применяет модели latency, market impact, fees и rejection, генерирует синтетические ExecutionReport в стандартном формате F-12 и публикует их в тот же Kafka-топик `execution.venue`. Всё остальное downstream (Ledger, Risk, ClickHouse) потребляет эти отчёты без изменений, однако каждый симулированный отчёт маркируется `simMode: true`.

**Параметры (как логической фичи):**

- `simScope` — перечень venues и инструментов, для которых включён sim-режим; остальные venues остаются в боевом (LIVE) режиме.
- `liveLobSource` — источник живого LOB: `venue.snapshots` (Kafka) или прямой pull из VenueSnapshot cache.
- `simModels` — набор подключённых моделей симуляции: `latency`, `impact`, `fee`, `rejection`.
- `routingMode` — режим роутинга child-ордеров: `SIM_ONLY`, `LIVE_ONLY`, `SHADOW` (dual-send: боевое + симулируемое параллельно).
- `dependencies` — зависимые фичи: F-11 (живой LOB), F-12 (источник ExecutionIntent), F-15 (переиспользование моделей), Kafka, PostgreSQL, ClickHouse.

---

#### VenueSimulator

**Определение**
Центральный компонент F-20. Принимает ChildOrderRequest от Venue Execution Adapter (F-12), загружает актуальный VenueSnapshot из живого источника F-11, вычисляет результат симулированного исполнения и через настраиваемую задержку (latency model) публикует синтетический ExecutionReport.

**Параметры VenueSimulator:**

- `simSessionId : UUID` — идентификатор текущей активной sim-сессии.
- `venueId : string` — venue, работу которой симулирует данный экземпляр (например, `binance_sim`).
- `latencyModel : LatencyModel` — параметры симуляции задержки (p50, p95, p99 мс; distribution: lognormal/uniform/fixed).
- `impactModel : ImpactModel` — модель market impact: LINEAR, SQRT, POWER_LAW.
- `feeModel : FeeModel` — ставки maker/taker, минимальная комиссия, gas (DEX); совместимы с форматом F-15.
- `rejectionModel : RejectionModel` — конфигурируемые сценарии отказа (нехватка ликвидности, price constraint, rate limit, random rejection rate).
- `staleLobThresholdMs : integer` — порог свежести VenueSnapshot; при превышении генерируется sim-таймаут SIM_STALE_LOB.
- `partialFillMode : string (PROPORTIONAL|LEVEL_BY_LEVEL|NONE)` — стратегия частичного исполнения.

---

#### SimSession

**Определение**
Конфигурационный объект одного активного периода работы F-20 для конкретного набора venues/инструментов. Хранится в PostgreSQL и определяет все параметры моделей для текущего sim-периода.

**Параметры SimSession (таблица `sim_sessions` в PostgreSQL):**

- `simSessionId : UUID` — первичный ключ сессии.
- `name : string` — человекочитаемое имя (например, `BTC_SIM_2026Q2`).
- `routingMode : ENUM(SIM_ONLY|LIVE_ONLY|SHADOW)` — режим роутинга child-ордеров.
- `scopeVenues : string[]` — список venues в sim-режиме.
- `scopeInstruments : string[]` — список инструментов в sim-режиме.
- `latencyModel : JSONB` — конфигурация latency model.
- `impactModel : JSONB` — конфигурация impact model.
- `feeModel : JSONB` — конфигурация fee model.
- `rejectionModel : JSONB` — конфигурация rejection model.
- `staleLobThresholdMs : integer` — порог свежести LOB.
- `partialFillMode : ENUM` — стратегия частичного исполнения.
- `status : ENUM(ACTIVE|PAUSED|COMPLETED|CANCELLED)` — статус сессии.
- `createdAt : timestamptz`, `activatedAt : timestamptz | null`, `completedAt : timestamptz | null`, `createdBy : string`.

---

#### ChildOrderRequest

**Определение**
Внутренний запрос на исполнение child-ордера, передаваемый от Venue Execution Adapter (F-12) в VenueSimRouter. Является стандартным контрактом независимо от режима (SIM/LIVE).

**Параметры:** `childOrderId`, `hedgeFlowId`, `venueId`, `symbol`, `side (BUY|SELL)`, `orderType (LIMIT|MARKET|IOC|POST_ONLY)`, `qty`, `price | null`, `timeInForce`, `clientOrderId`, `simSessionId | null`.

---

#### SimExecutionReport

**Определение**
Синтетический ExecutionReport, генерируемый VenueSimulator. Идентичен схеме боевого ExecutionReport (F-12) плюс дополнительные поля трассировки.

**Стандартные поля ExecutionReport:** `executionId`, `venueId`, `symbol`, `side`, `filledQty`, `avgPrice`, `fee`, `status (FILLED|PARTIALLY_FILLED|CANCELLED|REJECTED)`, `timestamp`, `clientOrderId`, `hedgeFlowId`.

**Расширенные поля симуляции:**
- `simMode : boolean = true`
- `simSessionId : UUID` — ссылка на SimSession.
- `lobSnapshotId : UUID` — ID VenueSnapshot, использованного для симуляции.
- `lobAge : integer` — возраст LOB на момент симуляции (мс).
- `impactBps : decimal` — рассчитанный market impact в базисных пунктах.
- `slippageBps : decimal` — итоговое отклонение от referenceMid.
- `latencySampleMs : integer` — сэмплированная задержка из LatencyModel.

---

#### VenueSimRouter

**Определение**
Маршрутизатор child-ордеров между VenueSimulator и реальным External Venues Connector. Вставляется между Venue Execution Adapter (F-12) и EVC. Режимы маршрутизации:

- `SIM_ONLY` — все child-ордера идут только в VenueSimulator; реальный EVC не вызывается.
- `LIVE_ONLY` — VenueSimRouter прозрачно проксирует запрос в EVC; симулятор не вызывается.
- `SHADOW` — ChildOrderRequest направляется одновременно в EVC (LIVE) и в VenueSimulator (SIM); оба ExecutionReport публикуются с соответствующими маркерами `simMode`. Позволяет сравнивать реальное и симулированное исполнение в live-режиме.

---

#### LatencyModel, ImpactModel, RejectionModel

| Сущность | Ключевые параметры |
|---|---|
| **LatencyModel** | `distribution (LOGNORMAL|UNIFORM|FIXED|EMPIRICAL)`, `p50Ms`, `p95Ms`, `p99Ms`, `tailProbability`, `timeoutMs` |
| **ImpactModel** | `modelType (LINEAR|SQRT|POWER_LAW|LEVEL_BY_LEVEL)`, `impactCoeff`, `powerExponent`, `depletionMode`, `depletionDecayMs` |
| **RejectionModel** | `insufficientLiquidityEnabled`, `priceConstraintEnabled`, `randomRejectionRate`, `rateLimitRejectRate`, `minLiquidityThreshold` |

Отвечу коротко и по существу, опираясь на текст F‑16. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/515efc2d-54b2-4584-8215-f2bbf8279944/F-20-Live-Venue-Simulator.md?AWSAccessKeyId=ASIA2F3EMEYEV7RSQBS2&Signature=lz%2Fo6KItbztcDn8NDYSYQwpi8sg%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEEEaCXVzLWVhc3QtMSJIMEYCIQDPeYko164P4E8eN5bikyHQlJx%2BUikBtNSg882HeNTwYwIhANydYR8F%2FI1uMp3AgiLTyUwBHPyydX9yxIoqaR8DsISJKvMECAkQARoMNjk5NzUzMzA5NzA1IgyRkCs2av%2Fh6YG4cYYq0AQZ8d5nKmGfFhqm5Nq51HbuVm2hzzL%2F8XkkB%2Bdz3jL3ONi%2B8kbPxEAK%2B8ofh2K3ptAPTbwQMcDrMBKLf56I1j6jcF8fVpyFWqqXE4aQq6%2FWjW3WMSC42HCgCnUmlv5mBW3hTlbN7q6zBDSaWH65E84Gjj7srVKTNrZV2cExDYCpn2OjqB8QRfaOWIRYsEIdNrs0NMYujEdT5Sz3OIDK4CdzGD058SPNUEsBPStyTrw0s1KMPWuOgCqKsm3xEtV0XFFod8Pd5L5C3vCW%2FZ77W%2BRmTT75QMZ9DLAMRqa3M5z9w0paJCQwJnMZ0TZJg3TY6czWunh%2Fur9Uv1Bwb1HK%2BjM1v1gqqBNH05Dphua585VWsa7UlCJdwu69OOBOEekVzzR1DWpfAz8h6HvN8DM7vXSqnO0Fzby%2BsKAvPmh7JacXPEqaUMcMndiBzlrlzDoc3wwjO1HziKy%2FoypHjk0lBLmgpm32ITPrVLHcooHIKveBsnFC%2FjR6%2F8pFGXvGzZ8aNawz1YLkhKHwoX97FSOO5m7reZM6j8nbzZmO9mlJH2EptY5%2BZrBH5gnQA%2B5CiLZnqmSRLdLg9Av2vksEmV4daF%2BU49PM78f3p%2Fz98iM1ObunVdlbHUf8gijNN5T3DDhmWBmiNqsrH8D%2FRl03BlIeGAWsLR3Ov%2FZjZKyeYLho%2FDDMeJyD11KyGR5lfHcQxCViMUjM%2FRlnY1rr8r%2BKlm%2B2f2wcbvHV6bR5ycl3PxECOoCt8v%2F89CxkTLvJWsrQ6%2FD9s7JcJwBI9jNC5E2mbfBons19MJ%2BezM8GOpcBbWzOju5cd31A3l1%2BGvlzlWJkHvToR8h92eOhz0ffflSs0jIYCDsprA6YEJEN%2FU%2FJcZN%2F4uWfGy5lbSEJojINACtYxhHaWXtq52M2iW%2Bu%2F0ABWR5%2FJRZab3mQaZJtcNSn%2BjG31QL7a3nXXPDmOS%2FNm7ayGD82srxmg06ip7BXiH4vO2shZ%2Br2KGWSfftf3Jm%2BAJBQd46NLQ%3D%3D&Expires=1777537654)

***

##### LatencyModel

LatencyModel описывает, **как долго «отвечает» внешняя площадка** в симуляции. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/515efc2d-54b2-4584-8215-f2bbf8279944/F-20-Live-Venue-Simulator.md?AWSAccessKeyId=ASIA2F3EMEYEV7RSQBS2&Signature=lz%2Fo6KItbztcDn8NDYSYQwpi8sg%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEEEaCXVzLWVhc3QtMSJIMEYCIQDPeYko164P4E8eN5bikyHQlJx%2BUikBtNSg882HeNTwYwIhANydYR8F%2FI1uMp3AgiLTyUwBHPyydX9yxIoqaR8DsISJKvMECAkQARoMNjk5NzUzMzA5NzA1IgyRkCs2av%2Fh6YG4cYYq0AQZ8d5nKmGfFhqm5Nq51HbuVm2hzzL%2F8XkkB%2Bdz3jL3ONi%2B8kbPxEAK%2B8ofh2K3ptAPTbwQMcDrMBKLf56I1j6jcF8fVpyFWqqXE4aQq6%2FWjW3WMSC42HCgCnUmlv5mBW3hTlbN7q6zBDSaWH65E84Gjj7srVKTNrZV2cExDYCpn2OjqB8QRfaOWIRYsEIdNrs0NMYujEdT5Sz3OIDK4CdzGD058SPNUEsBPStyTrw0s1KMPWuOgCqKsm3xEtV0XFFod8Pd5L5C3vCW%2FZ77W%2BRmTT75QMZ9DLAMRqa3M5z9w0paJCQwJnMZ0TZJg3TY6czWunh%2Fur9Uv1Bwb1HK%2BjM1v1gqqBNH05Dphua585VWsa7UlCJdwu69OOBOEekVzzR1DWpfAz8h6HvN8DM7vXSqnO0Fzby%2BsKAvPmh7JacXPEqaUMcMndiBzlrlzDoc3wwjO1HziKy%2FoypHjk0lBLmgpm32ITPrVLHcooHIKveBsnFC%2FjR6%2F8pFGXvGzZ8aNawz1YLkhKHwoX97FSOO5m7reZM6j8nbzZmO9mlJH2EptY5%2BZrBH5gnQA%2B5CiLZnqmSRLdLg9Av2vksEmV4daF%2BU49PM78f3p%2Fz98iM1ObunVdlbHUf8gijNN5T3DDhmWBmiNqsrH8D%2FRl03BlIeGAWsLR3Ov%2FZjZKyeYLho%2FDDMeJyD11KyGR5lfHcQxCViMUjM%2FRlnY1rr8r%2BKlm%2B2f2wcbvHV6bR5ycl3PxECOoCt8v%2F89CxkTLvJWsrQ6%2FD9s7JcJwBI9jNC5E2mbfBons19MJ%2BezM8GOpcBbWzOju5cd31A3l1%2BGvlzlWJkHvToR8h92eOhz0ffflSs0jIYCDsprA6YEJEN%2FU%2FJcZN%2F4uWfGy5lbSEJojINACtYxhHaWXtq52M2iW%2Bu%2F0ABWR5%2FJRZab3mQaZJtcNSn%2BjG31QL7a3nXXPDmOS%2FNm7ayGD82srxmg06ip7BXiH4vO2shZ%2Br2KGWSfftf3Jm%2BAJBQd46NLQ%3D%3D&Expires=1777537654)

- Хранит форму распределения задержки: `distribution (LOGNORMAL|UNIFORM|FIXED|EMPIRICAL)` и целевые перцентили `p50Ms`, `p95Ms`, `p99Ms`, а также `tailProbability` для «длинного хвоста» и `timeoutMs` как верхнюю границу. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/515efc2d-54b2-4584-8215-f2bbf8279944/F-20-Live-Venue-Simulator.md?AWSAccessKeyId=ASIA2F3EMEYEV7RSQBS2&Signature=lz%2Fo6KItbztcDn8NDYSYQwpi8sg%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEEEaCXVzLWVhc3QtMSJIMEYCIQDPeYko164P4E8eN5bikyHQlJx%2BUikBtNSg882HeNTwYwIhANydYR8F%2FI1uMp3AgiLTyUwBHPyydX9yxIoqaR8DsISJKvMECAkQARoMNjk5NzUzMzA5NzA1IgyRkCs2av%2Fh6YG4cYYq0AQZ8d5nKmGfFhqm5Nq51HbuVm2hzzL%2F8XkkB%2Bdz3jL3ONi%2B8kbPxEAK%2B8ofh2K3ptAPTbwQMcDrMBKLf56I1j6jcF8fVpyFWqqXE4aQq6%2FWjW3WMSC42HCgCnUmlv5mBW3hTlbN7q6zBDSaWH65E84Gjj7srVKTNrZV2cExDYCpn2OjqB8QRfaOWIRYsEIdNrs0NMYujEdT5Sz3OIDK4CdzGD058SPNUEsBPStyTrw0s1KMPWuOgCqKsm3xEtV0XFFod8Pd5L5C3vCW%2FZ77W%2BRmTT75QMZ9DLAMRqa3M5z9w0paJCQwJnMZ0TZJg3TY6czWunh%2Fur9Uv1Bwb1HK%2BjM1v1gqqBNH05Dphua585VWsa7UlCJdwu69OOBOEekVzzR1DWpfAz8h6HvN8DM7vXSqnO0Fzby%2BsKAvPmh7JacXPEqaUMcMndiBzlrlzDoc3wwjO1HziKy%2FoypHjk0lBLmgpm32ITPrVLHcooHIKveBsnFC%2FjR6%2F8pFGXvGzZ8aNawz1YLkhKHwoX97FSOO5m7reZM6j8nbzZmO9mlJH2EptY5%2BZrBH5gnQA%2B5CiLZnqmSRLdLg9Av2vksEmV4daF%2BU49PM78f3p%2Fz98iM1ObunVdlbHUf8gijNN5T3DDhmWBmiNqsrH8D%2FRl03BlIeGAWsLR3Ov%2FZjZKyeYLho%2FDDMeJyD11KyGR5lfHcQxCViMUjM%2FRlnY1rr8r%2BKlm%2B2f2wcbvHV6bR5ycl3PxECOoCt8v%2F89CxkTLvJWsrQ6%2FD9s7JcJwBI9jNC5E2mbfBons19MJ%2BezM8GOpcBbWzOju5cd31A3l1%2BGvlzlWJkHvToR8h92eOhz0ffflSs0jIYCDsprA6YEJEN%2FU%2FJcZN%2F4uWfGy5lbSEJojINACtYxhHaWXtq52M2iW%2Bu%2F0ABWR5%2FJRZab3mQaZJtcNSn%2BjG31QL7a3nXXPDmOS%2FNm7ayGD82srxmg06ip7BXiH4vO2shZ%2Br2KGWSfftf3Jm%2BAJBQd46NLQ%3D%3D&Expires=1777537654)
- При каждом child‑ордере симулятор берёт сэмпл задержки из этой модели и асинхронно «задерживает» публикацию SimExecutionReport на `latencySampleMs`, как если бы ордер реально прошёл по сети до биржи и обратно. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/515efc2d-54b2-4584-8215-f2bbf8279944/F-20-Live-Venue-Simulator.md?AWSAccessKeyId=ASIA2F3EMEYEV7RSQBS2&Signature=lz%2Fo6KItbztcDn8NDYSYQwpi8sg%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEEEaCXVzLWVhc3QtMSJIMEYCIQDPeYko164P4E8eN5bikyHQlJx%2BUikBtNSg882HeNTwYwIhANydYR8F%2FI1uMp3AgiLTyUwBHPyydX9yxIoqaR8DsISJKvMECAkQARoMNjk5NzUzMzA5NzA1IgyRkCs2av%2Fh6YG4cYYq0AQZ8d5nKmGfFhqm5Nq51HbuVm2hzzL%2F8XkkB%2Bdz3jL3ONi%2B8kbPxEAK%2B8ofh2K3ptAPTbwQMcDrMBKLf56I1j6jcF8fVpyFWqqXE4aQq6%2FWjW3WMSC42HCgCnUmlv5mBW3hTlbN7q6zBDSaWH65E84Gjj7srVKTNrZV2cExDYCpn2OjqB8QRfaOWIRYsEIdNrs0NMYujEdT5Sz3OIDK4CdzGD058SPNUEsBPStyTrw0s1KMPWuOgCqKsm3xEtV0XFFod8Pd5L5C3vCW%2FZ77W%2BRmTT75QMZ9DLAMRqa3M5z9w0paJCQwJnMZ0TZJg3TY6czWunh%2Fur9Uv1Bwb1HK%2BjM1v1gqqBNH05Dphua585VWsa7UlCJdwu69OOBOEekVzzR1DWpfAz8h6HvN8DM7vXSqnO0Fzby%2BsKAvPmh7JacXPEqaUMcMndiBzlrlzDoc3wwjO1HziKy%2FoypHjk0lBLmgpm32ITPrVLHcooHIKveBsnFC%2FjR6%2F8pFGXvGzZ8aNawz1YLkhKHwoX97FSOO5m7reZM6j8nbzZmO9mlJH2EptY5%2BZrBH5gnQA%2B5CiLZnqmSRLdLg9Av2vksEmV4daF%2BU49PM78f3p%2Fz98iM1ObunVdlbHUf8gijNN5T3DDhmWBmiNqsrH8D%2FRl03BlIeGAWsLR3Ov%2FZjZKyeYLho%2FDDMeJyD11KyGR5lfHcQxCViMUjM%2FRlnY1rr8r%2BKlm%2B2f2wcbvHV6bR5ycl3PxECOoCt8v%2F89CxkTLvJWsrQ6%2FD9s7JcJwBI9jNC5E2mbfBons19MJ%2BezM8GOpcBbWzOju5cd31A3l1%2BGvlzlWJkHvToR8h92eOhz0ffflSs0jIYCDsprA6YEJEN%2FU%2FJcZN%2F4uWfGy5lbSEJojINACtYxhHaWXtq52M2iW%2Bu%2F0ABWR5%2FJRZab3mQaZJtcNSn%2BjG31QL7a3nXXPDmOS%2FNm7ayGD82srxmg06ip7BXiH4vO2shZ%2Br2KGWSfftf3Jm%2BAJBQd46NLQ%3D%3D&Expires=1777537654)
- Если сэмпл превышает `timeoutMs`, симулятор вместо FILLED/FILLED_PARTIAL возвращает отказ типа `SIM_TIMEOUT`, что позволяет тестировать поведение F‑12/F‑11 при зависаниях внешнего venue. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/515efc2d-54b2-4584-8215-f2bbf8279944/F-20-Live-Venue-Simulator.md?AWSAccessKeyId=ASIA2F3EMEYEV7RSQBS2&Signature=lz%2Fo6KItbztcDn8NDYSYQwpi8sg%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEEEaCXVzLWVhc3QtMSJIMEYCIQDPeYko164P4E8eN5bikyHQlJx%2BUikBtNSg882HeNTwYwIhANydYR8F%2FI1uMp3AgiLTyUwBHPyydX9yxIoqaR8DsISJKvMECAkQARoMNjk5NzUzMzA5NzA1IgyRkCs2av%2Fh6YG4cYYq0AQZ8d5nKmGfFhqm5Nq51HbuVm2hzzL%2F8XkkB%2Bdz3jL3ONi%2B8kbPxEAK%2B8ofh2K3ptAPTbwQMcDrMBKLf56I1j6jcF8fVpyFWqqXE4aQq6%2FWjW3WMSC42HCgCnUmlv5mBW3hTlbN7q6zBDSaWH65E84Gjj7srVKTNrZV2cExDYCpn2OjqB8QRfaOWIRYsEIdNrs0NMYujEdT5Sz3OIDK4CdzGD058SPNUEsBPStyTrw0s1KMPWuOgCqKsm3xEtV0XFFod8Pd5L5C3vCW%2FZ77W%2BRmTT75QMZ9DLAMRqa3M5z9w0paJCQwJnMZ0TZJg3TY6czWunh%2Fur9Uv1Bwb1HK%2BjM1v1gqqBNH05Dphua585VWsa7UlCJdwu69OOBOEekVzzR1DWpfAz8h6HvN8DM7vXSqnO0Fzby%2BsKAvPmh7JacXPEqaUMcMndiBzlrlzDoc3wwjO1HziKy%2FoypHjk0lBLmgpm32ITPrVLHcooHIKveBsnFC%2FjR6%2F8pFGXvGzZ8aNawz1YLkhKHwoX97FSOO5m7reZM6j8nbzZmO9mlJH2EptY5%2BZrBH5gnQA%2B5CiLZnqmSRLdLg9Av2vksEmV4daF%2BU49PM78f3p%2Fz98iM1ObunVdlbHUf8gijNN5T3DDhmWBmiNqsrH8D%2FRl03BlIeGAWsLR3Ov%2FZjZKyeYLho%2FDDMeJyD11KyGR5lfHcQxCViMUjM%2FRlnY1rr8r%2BKlm%2B2f2wcbvHV6bR5ycl3PxECOoCt8v%2F89CxkTLvJWsrQ6%2FD9s7JcJwBI9jNC5E2mbfBons19MJ%2BezM8GOpcBbWzOju5cd31A3l1%2BGvlzlWJkHvToR8h92eOhz0ffflSs0jIYCDsprA6YEJEN%2FU%2FJcZN%2F4uWfGy5lbSEJojINACtYxhHaWXtq52M2iW%2Bu%2F0ABWR5%2FJRZab3mQaZJtcNSn%2BjG31QL7a3nXXPDmOS%2FNm7ayGD82srxmg06ip7BXiH4vO2shZ%2Br2KGWSfftf3Jm%2BAJBQd46NLQ%3D%3D&Expires=1777537654)

Интуитивно LatencyModel — это «распределение сетевой/биржевой задержки», которое вы можете настраивать и калибровать, например по фактическим метрикам из SHADOW‑режима.

***

##### ImpactModel

ImpactModel описывает, **как объём ордера влияет на цену исполнения**, то есть market impact. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/515efc2d-54b2-4584-8215-f2bbf8279944/F-20-Live-Venue-Simulator.md?AWSAccessKeyId=ASIA2F3EMEYEV7RSQBS2&Signature=lz%2Fo6KItbztcDn8NDYSYQwpi8sg%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEEEaCXVzLWVhc3QtMSJIMEYCIQDPeYko164P4E8eN5bikyHQlJx%2BUikBtNSg882HeNTwYwIhANydYR8F%2FI1uMp3AgiLTyUwBHPyydX9yxIoqaR8DsISJKvMECAkQARoMNjk5NzUzMzA5NzA1IgyRkCs2av%2Fh6YG4cYYq0AQZ8d5nKmGfFhqm5Nq51HbuVm2hzzL%2F8XkkB%2Bdz3jL3ONi%2B8kbPxEAK%2B8ofh2K3ptAPTbwQMcDrMBKLf56I1j6jcF8fVpyFWqqXE4aQq6%2FWjW3WMSC42HCgCnUmlv5mBW3hTlbN7q6zBDSaWH65E84Gjj7srVKTNrZV2cExDYCpn2OjqB8QRfaOWIRYsEIdNrs0NMYujEdT5Sz3OIDK4CdzGD058SPNUEsBPStyTrw0s1KMPWuOgCqKsm3xEtV0XFFod8Pd5L5C3vCW%2FZ77W%2BRmTT75QMZ9DLAMRqa3M5z9w0paJCQwJnMZ0TZJg3TY6czWunh%2Fur9Uv1Bwb1HK%2BjM1v1gqqBNH05Dphua585VWsa7UlCJdwu69OOBOEekVzzR1DWpfAz8h6HvN8DM7vXSqnO0Fzby%2BsKAvPmh7JacXPEqaUMcMndiBzlrlzDoc3wwjO1HziKy%2FoypHjk0lBLmgpm32ITPrVLHcooHIKveBsnFC%2FjR6%2F8pFGXvGzZ8aNawz1YLkhKHwoX97FSOO5m7reZM6j8nbzZmO9mlJH2EptY5%2BZrBH5gnQA%2B5CiLZnqmSRLdLg9Av2vksEmV4daF%2BU49PM78f3p%2Fz98iM1ObunVdlbHUf8gijNN5T3DDhmWBmiNqsrH8D%2FRl03BlIeGAWsLR3Ov%2FZjZKyeYLho%2FDDMeJyD11KyGR5lfHcQxCViMUjM%2FRlnY1rr8r%2BKlm%2B2f2wcbvHV6bR5ycl3PxECOoCt8v%2F89CxkTLvJWsrQ6%2FD9s7JcJwBI9jNC5E2mbfBons19MJ%2BezM8GOpcBbWzOju5cd31A3l1%2BGvlzlWJkHvToR8h92eOhz0ffflSs0jIYCDsprA6YEJEN%2FU%2FJcZN%2F4uWfGy5lbSEJojINACtYxhHaWXtq52M2iW%2Bu%2F0ABWR5%2FJRZab3mQaZJtcNSn%2BjG31QL7a3nXXPDmOS%2FNm7ayGD82srxmg06ip7BXiH4vO2shZ%2Br2KGWSfftf3Jm%2BAJBQd46NLQ%3D%3D&Expires=1777537654)

- Модель получает на вход объём `filledQty` и структуру LOB, и по выбранному типу (`modelType = LINEAR|SQRT|POWER_LAW|LEVEL_BY_LEVEL`) считает сдвиг цены $\Delta p$относительно VWAP по стакану.
- Примеры (из спецификации): LINEAR $\Delta p = \alpha \cdot \text{filledQty} \), SQRT \( \Delta p = \alpha \cdot \sqrt{\text{filledQty}}$ ,  дальше считается скорректированная цена $\text{avgPrice}_{impact} = \text{avgPrice} \pm \Delta p$ и метрика `impactBps` в bps относительно mid. 
- Параметры включают `impactCoeff`, `powerExponent` и флаги `depletionMode`/`depletionDecayMs`, которые управляют тем, «продаёт» ли симулятор ликвидность из LOB (depletion) и на сколько быстро она «восстанавливается» в state симулятора. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/515efc2d-54b2-4584-8215-f2bbf8279944/F-20-Live-Venue-Simulator.md?AWSAccessKeyId=ASIA2F3EMEYEV7RSQBS2&Signature=lz%2Fo6KItbztcDn8NDYSYQwpi8sg%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEEEaCXVzLWVhc3QtMSJIMEYCIQDPeYko164P4E8eN5bikyHQlJx%2BUikBtNSg882HeNTwYwIhANydYR8F%2FI1uMp3AgiLTyUwBHPyydX9yxIoqaR8DsISJKvMECAkQARoMNjk5NzUzMzA5NzA1IgyRkCs2av%2Fh6YG4cYYq0AQZ8d5nKmGfFhqm5Nq51HbuVm2hzzL%2F8XkkB%2Bdz3jL3ONi%2B8kbPxEAK%2B8ofh2K3ptAPTbwQMcDrMBKLf56I1j6jcF8fVpyFWqqXE4aQq6%2FWjW3WMSC42HCgCnUmlv5mBW3hTlbN7q6zBDSaWH65E84Gjj7srVKTNrZV2cExDYCpn2OjqB8QRfaOWIRYsEIdNrs0NMYujEdT5Sz3OIDK4CdzGD058SPNUEsBPStyTrw0s1KMPWuOgCqKsm3xEtV0XFFod8Pd5L5C3vCW%2FZ77W%2BRmTT75QMZ9DLAMRqa3M5z9w0paJCQwJnMZ0TZJg3TY6czWunh%2Fur9Uv1Bwb1HK%2BjM1v1gqqBNH05Dphua585VWsa7UlCJdwu69OOBOEekVzzR1DWpfAz8h6HvN8DM7vXSqnO0Fzby%2BsKAvPmh7JacXPEqaUMcMndiBzlrlzDoc3wwjO1HziKy%2FoypHjk0lBLmgpm32ITPrVLHcooHIKveBsnFC%2FjR6%2F8pFGXvGzZ8aNawz1YLkhKHwoX97FSOO5m7reZM6j8nbzZmO9mlJH2EptY5%2BZrBH5gnQA%2B5CiLZnqmSRLdLg9Av2vksEmV4daF%2BU49PM78f3p%2Fz98iM1ObunVdlbHUf8gijNN5T3DDhmWBmiNqsrH8D%2FRl03BlIeGAWsLR3Ov%2FZjZKyeYLho%2FDDMeJyD11KyGR5lfHcQxCViMUjM%2FRlnY1rr8r%2BKlm%2B2f2wcbvHV6bR5ycl3PxECOoCt8v%2F89CxkTLvJWsrQ6%2FD9s7JcJwBI9jNC5E2mbfBons19MJ%2BezM8GOpcBbWzOju5cd31A3l1%2BGvlzlWJkHvToR8h92eOhz0ffflSs0jIYCDsprA6YEJEN%2FU%2FJcZN%2F4uWfGy5lbSEJojINACtYxhHaWXtq52M2iW%2Bu%2F0ABWR5%2FJRZab3mQaZJtcNSn%2BjG31QL7a3nXXPDmOS%2FNm7ayGD82srxmg06ip7BXiH4vO2shZ%2Br2KGWSfftf3Jm%2BAJBQd46NLQ%3D%3D&Expires=1777537654)

По сути ImpactModel — это модуль, который превращает «механический» LOB‑матчинг в реалистичное исполнение с учётом того, что крупный ордер двигает рынок.

***

##### RejectionModel

RejectionModel определяет, **когда симулятор должен вернуть отказ вместо исполнения**, имитируя реальные причины REJECTED на бирже. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/515efc2d-54b2-4584-8215-f2bbf8279944/F-20-Live-Venue-Simulator.md?AWSAccessKeyId=ASIA2F3EMEYEV7RSQBS2&Signature=lz%2Fo6KItbztcDn8NDYSYQwpi8sg%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEEEaCXVzLWVhc3QtMSJIMEYCIQDPeYko164P4E8eN5bikyHQlJx%2BUikBtNSg882HeNTwYwIhANydYR8F%2FI1uMp3AgiLTyUwBHPyydX9yxIoqaR8DsISJKvMECAkQARoMNjk5NzUzMzA5NzA1IgyRkCs2av%2Fh6YG4cYYq0AQZ8d5nKmGfFhqm5Nq51HbuVm2hzzL%2F8XkkB%2Bdz3jL3ONi%2B8kbPxEAK%2B8ofh2K3ptAPTbwQMcDrMBKLf56I1j6jcF8fVpyFWqqXE4aQq6%2FWjW3WMSC42HCgCnUmlv5mBW3hTlbN7q6zBDSaWH65E84Gjj7srVKTNrZV2cExDYCpn2OjqB8QRfaOWIRYsEIdNrs0NMYujEdT5Sz3OIDK4CdzGD058SPNUEsBPStyTrw0s1KMPWuOgCqKsm3xEtV0XFFod8Pd5L5C3vCW%2FZ77W%2BRmTT75QMZ9DLAMRqa3M5z9w0paJCQwJnMZ0TZJg3TY6czWunh%2Fur9Uv1Bwb1HK%2BjM1v1gqqBNH05Dphua585VWsa7UlCJdwu69OOBOEekVzzR1DWpfAz8h6HvN8DM7vXSqnO0Fzby%2BsKAvPmh7JacXPEqaUMcMndiBzlrlzDoc3wwjO1HziKy%2FoypHjk0lBLmgpm32ITPrVLHcooHIKveBsnFC%2FjR6%2F8pFGXvGzZ8aNawz1YLkhKHwoX97FSOO5m7reZM6j8nbzZmO9mlJH2EptY5%2BZrBH5gnQA%2B5CiLZnqmSRLdLg9Av2vksEmV4daF%2BU49PM78f3p%2Fz98iM1ObunVdlbHUf8gijNN5T3DDhmWBmiNqsrH8D%2FRl03BlIeGAWsLR3Ov%2FZjZKyeYLho%2FDDMeJyD11KyGR5lfHcQxCViMUjM%2FRlnY1rr8r%2BKlm%2B2f2wcbvHV6bR5ycl3PxECOoCt8v%2F89CxkTLvJWsrQ6%2FD9s7JcJwBI9jNC5E2mbfBons19MJ%2BezM8GOpcBbWzOju5cd31A3l1%2BGvlzlWJkHvToR8h92eOhz0ffflSs0jIYCDsprA6YEJEN%2FU%2FJcZN%2F4uWfGy5lbSEJojINACtYxhHaWXtq52M2iW%2Bu%2F0ABWR5%2FJRZab3mQaZJtcNSn%2BjG31QL7a3nXXPDmOS%2FNm7ayGD82srxmg06ip7BXiH4vO2shZ%2Br2KGWSfftf3Jm%2BAJBQd46NLQ%3D%3D&Expires=1777537654)

- Включает набор бинарных флагов и вероятностей:  
  - `insufficientLiquidityEnabled` — отклонять ордер, если в стакане недостаточно объёма,  
  - `priceConstraintEnabled` — отклонять при нарушении лимитной/constraint‑цены из ExecutionIntent,  
  - `randomRejectionRate` — вероятность случайного отказа (симуляция «шумных» ошибок venue),  
  - `rateLimitRejectRate` — вероятность отказа по rate‑limit,  
  - `minLiquidityThreshold` — порог ликвидности, ниже которого ордер автоматически отвергается. [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/515efc2d-54b2-4584-8215-f2bbf8279944/F-20-Live-Venue-Simulator.md?AWSAccessKeyId=ASIA2F3EMEYEV7RSQBS2&Signature=lz%2Fo6KItbztcDn8NDYSYQwpi8sg%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEEEaCXVzLWVhc3QtMSJIMEYCIQDPeYko164P4E8eN5bikyHQlJx%2BUikBtNSg882HeNTwYwIhANydYR8F%2FI1uMp3AgiLTyUwBHPyydX9yxIoqaR8DsISJKvMECAkQARoMNjk5NzUzMzA5NzA1IgyRkCs2av%2Fh6YG4cYYq0AQZ8d5nKmGfFhqm5Nq51HbuVm2hzzL%2F8XkkB%2Bdz3jL3ONi%2B8kbPxEAK%2B8ofh2K3ptAPTbwQMcDrMBKLf56I1j6jcF8fVpyFWqqXE4aQq6%2FWjW3WMSC42HCgCnUmlv5mBW3hTlbN7q6zBDSaWH65E84Gjj7srVKTNrZV2cExDYCpn2OjqB8QRfaOWIRYsEIdNrs0NMYujEdT5Sz3OIDK4CdzGD058SPNUEsBPStyTrw0s1KMPWuOgCqKsm3xEtV0XFFod8Pd5L5C3vCW%2FZ77W%2BRmTT75QMZ9DLAMRqa3M5z9w0paJCQwJnMZ0TZJg3TY6czWunh%2Fur9Uv1Bwb1HK%2BjM1v1gqqBNH05Dphua585VWsa7UlCJdwu69OOBOEekVzzR1DWpfAz8h6HvN8DM7vXSqnO0Fzby%2BsKAvPmh7JacXPEqaUMcMndiBzlrlzDoc3wwjO1HziKy%2FoypHjk0lBLmgpm32ITPrVLHcooHIKveBsnFC%2FjR6%2F8pFGXvGzZ8aNawz1YLkhKHwoX97FSOO5m7reZM6j8nbzZmO9mlJH2EptY5%2BZrBH5gnQA%2B5CiLZnqmSRLdLg9Av2vksEmV4daF%2BU49PM78f3p%2Fz98iM1ObunVdlbHUf8gijNN5T3DDhmWBmiNqsrH8D%2FRl03BlIeGAWsLR3Ov%2FZjZKyeYLho%2FDDMeJyD11KyGR5lfHcQxCViMUjM%2FRlnY1rr8r%2BKlm%2B2f2wcbvHV6bR5ycl3PxECOoCt8v%2F89CxkTLvJWsrQ6%2FD9s7JcJwBI9jNC5E2mbfBons19MJ%2BezM8GOpcBbWzOju5cd31A3l1%2BGvlzlWJkHvToR8h92eOhz0ffflSs0jIYCDsprA6YEJEN%2FU%2FJcZN%2F4uWfGy5lbSEJojINACtYxhHaWXtq52M2iW%2Bu%2F0ABWR5%2FJRZab3mQaZJtcNSn%2BjG31QL7a3nXXPDmOS%2FNm7ayGD82srxmg06ip7BXiH4vO2shZ%2Br2KGWSfftf3Jm%2BAJBQd46NLQ%3D%3D&Expires=1777537654)
- После расчёта потенциального fill‑а VenueSimulator прогоняет результат через RejectionModel и либо подтверждает исполнение (FILLED / PARTIALLY_FILLED), либо выдаёт один из симулированных отказов (`SIM_NO_LIQUIDITY`, `SIM_RANDOM_REJECT`, `SIM_TIMEOUT`, `SIM_STALE_LOB` и т.д.). [ppl-ai-file-upload.s3.amazonaws](https://ppl-ai-file-upload.s3.amazonaws.com/web/direct-files/attachments/107971738/515efc2d-54b2-4584-8215-f2bbf8279944/F-20-Live-Venue-Simulator.md?AWSAccessKeyId=ASIA2F3EMEYEV7RSQBS2&Signature=lz%2Fo6KItbztcDn8NDYSYQwpi8sg%3D&x-amz-security-token=IQoJb3JpZ2luX2VjEEEaCXVzLWVhc3QtMSJIMEYCIQDPeYko164P4E8eN5bikyHQlJx%2BUikBtNSg882HeNTwYwIhANydYR8F%2FI1uMp3AgiLTyUwBHPyydX9yxIoqaR8DsISJKvMECAkQARoMNjk5NzUzMzA5NzA1IgyRkCs2av%2Fh6YG4cYYq0AQZ8d5nKmGfFhqm5Nq51HbuVm2hzzL%2F8XkkB%2Bdz3jL3ONi%2B8kbPxEAK%2B8ofh2K3ptAPTbwQMcDrMBKLf56I1j6jcF8fVpyFWqqXE4aQq6%2FWjW3WMSC42HCgCnUmlv5mBW3hTlbN7q6zBDSaWH65E84Gjj7srVKTNrZV2cExDYCpn2OjqB8QRfaOWIRYsEIdNrs0NMYujEdT5Sz3OIDK4CdzGD058SPNUEsBPStyTrw0s1KMPWuOgCqKsm3xEtV0XFFod8Pd5L5C3vCW%2FZ77W%2BRmTT75QMZ9DLAMRqa3M5z9w0paJCQwJnMZ0TZJg3TY6czWunh%2Fur9Uv1Bwb1HK%2BjM1v1gqqBNH05Dphua585VWsa7UlCJdwu69OOBOEekVzzR1DWpfAz8h6HvN8DM7vXSqnO0Fzby%2BsKAvPmh7JacXPEqaUMcMndiBzlrlzDoc3wwjO1HziKy%2FoypHjk0lBLmgpm32ITPrVLHcooHIKveBsnFC%2FjR6%2F8pFGXvGzZ8aNawz1YLkhKHwoX97FSOO5m7reZM6j8nbzZmO9mlJH2EptY5%2BZrBH5gnQA%2B5CiLZnqmSRLdLg9Av2vksEmV4daF%2BU49PM78f3p%2Fz98iM1ObunVdlbHUf8gijNN5T3DDhmWBmiNqsrH8D%2FRl03BlIeGAWsLR3Ov%2FZjZKyeYLho%2FDDMeJyD11KyGR5lfHcQxCViMUjM%2FRlnY1rr8r%2BKlm%2B2f2wcbvHV6bR5ycl3PxECOoCt8v%2F89CxkTLvJWsrQ6%2FD9s7JcJwBI9jNC5E2mbfBons19MJ%2BezM8GOpcBbWzOju5cd31A3l1%2BGvlzlWJkHvToR8h92eOhz0ffflSs0jIYCDsprA6YEJEN%2FU%2FJcZN%2F4uWfGy5lbSEJojINACtYxhHaWXtq52M2iW%2Bu%2F0ABWR5%2FJRZab3mQaZJtcNSn%2BjG31QL7a3nXXPDmOS%2FNm7ayGD82srxmg06ip7BXiH4vO2shZ%2Br2KGWSfftf3Jm%2BAJBQd46NLQ%3D%3D&Expires=1777537654)

Таким образом RejectionModel даёт контролируемый способ проверять, как вся цепочка F‑11/F‑12/F‑16/F‑15 реагирует на разные классы отказов внешних площадок, без реальных инцидентов.




**Глоссарий F-20:**F-20

| Термин | Определение |
|---|---|
| VenueSimulator | Ядро симуляции: принимает ChildOrderRequest, использует живой LOB, генерирует SimExecutionReport |
| SimSession | Конфигурационная сессия с набором активных моделей и областью применения |
| VenueSimRouter | Роутер child-ордеров между SIM и LIVE режимами |
| SimExecutionReport | Синтетический ExecutionReport с маркером simMode=true |
| LatencyModel | Модель задержки ответа симулируемой площадки |
| ImpactModel | Модель market impact (влияние ордера на цену из стакана) |
| RejectionModel | Модель отказов: нехватка ликвидности, rate limit, случайный reject |
| LOB Depletion | Симуляция поедания уровней стакана последовательными ордерами |
| SHADOW mode | Режим двойной отправки: боевое + симулированное исполнение параллельно |
| Sim Parity | Гарантия идентичности логики симуляции и реального исполнения по контракту |
| Divergence | Расхождение между SIM и LIVE исполнением в SHADOW-режиме |

### 1.1. Краткое описание фичи

**F-20 Live Venue Simulator** — фича, обеспечивающая возможность симуляции действий внешних площадок в ответ на child-ордера непрерывной биржи при сохранении **боевого приёма живых рыночных данных**. Система продолжает получать реальные LOB-стаканы от внешних CEX/DEX/AMM через F-11-инфраструктуру (External Venues Connector → Venue Market Data Normalizer → VenueSnapshot → `venue.snapshots`). Параллельно компонент **VenueSimRouter** перехватывает исходящие child-ордера от Venue Execution Adapter (F-12) и вместо отправки на реальную биржу передаёт их в **VenueSimulator**. Симулятор использует актуальный живой VenueSnapshot, применяет модели latency, market impact, fees и rejection, возвращает реалистичный синтетический ExecutionReport в стандартном формате F-12.

**Ключевые концепции:**

- **Гибридный режим (SIM_ONLY / SHADOW / LIVE_ONLY)** — система может одновременно работать в разных режимах для разных venues/инструментов. Режим SHADOW позволяет сравнивать реальное и симулированное исполнение в одном потоке.
- **Живой LOB как основа симуляции** — в отличие от F-15 (Backtest/Replay), VenueSimulator работает с актуальным, а не историческим стаканом. Это обеспечивает максимальную близость к реальным рыночным условиям.
- **SimSession** — конфигурационный объект, управляющий набором активных моделей и областью применения симуляции.
- **VenueSimRouter** — единственная точка переключения SIM/LIVE; Venue Execution Adapter не знает, куда направлен ChildOrderRequest.
- **SimExecutionReport** — стандартный ExecutionReport, расширенный полями трассировки симуляции.

### 1.2. Вовлечённые компоненты

- **External Venues Connector** (F-11) — боевой источник живых LOB; не модифицируется.
- **Venue Market Data Normalizer** (F-11) — поставляет VenueSnapshot в `venue.snapshots`; не модифицируется.
- **Venue Health & Routing Service** (F-11) — health-score влияет на параметры rejection model.
- **Venue Execution Adapter** (F-12) — источник ChildOrderRequest; не знает о режиме маршрутизации.
- **VenueSimRouter** (F-20, новый) — маршрутизатор SIM/LIVE/SHADOW.
- **VenueSimulator** (F-20, новый) — ядро симуляции: LOB-matching, impact, latency, fee, rejection.
- **SimSession Manager** (F-20, новый) — управление жизненным циклом SimSession.
- **Divergence Service** (F-20, новый) — сравнение LIVE vs SIM в SHADOW-режиме.
- **Kafka `venue.snapshots`** — источник живых VenueSnapshot для VenueSimulator.
- **Kafka `execution.venue`** — приёмник SimExecutionReport (тот же топик, что и для боевых исполнений).
- **Kafka `sim.execution.venue`** — опциональный изолированный топик только для симулированных отчётов.
- **Kafka `sim.config`** — топик событий изменения конфигурации SimSession (hot reload).
- **Kafka `sim.alerts`** — топик алертов симулятора (STALE_LOB, SIM_TIMEOUT, LOB_SOURCE_DOWN).
- **Settlement Ledger / Collateral Manager** — при `simMode=true` обновляет изолированную sim-книгу позиций.
- **Risk Manager** — получает SimExecutionReport для анализа sim-риска и мониторинга расхождений.
- **ClickHouse** — `sim_execution_reports`, `sim_divergence_log`.
- **PostgreSQL** — `sim_sessions`, `sim_child_orders`.
- **Observability & Reporting** — дашборды Sim Monitor, Sim vs Live, LOB Quality, Impact Analysis.
- **Admin UI / API Gateway** — управление SimSession, Model Calibration Panel.
- **Backtest Replay Engine** (F-15) — переиспользуются форматы LatencyModel и FeeModel.
- **Execution Planning & Forecast** (F-11/F-12) — может использовать SimExecutionReport для онлайн-калибровки.

### 1.3. Сценарии использования

1. **SIM_ONLY — pre-production проверка стратегии хеджа:**
   Оператор создаёт SimSession с `routingMode=SIM_ONLY` для BTC/USDT на Binance. F-11 продолжает принимать живой LOB. После батча F-12 генерирует ExecutionIntent → VenueSimRouter направляет ChildOrderRequest в VenueSimulator → симулятор использует актуальный VenueSnapshot, рассчитывает filledQty + avgPrice + fee → публикует SimExecutionReport → Ledger обновляет sim-книгу. Оператор получает реалистичные результаты без реального исполнения.

2. **SHADOW — параллельное сравнение:**
   Каждый child-ордер направляется одновременно на реальную биржу (LIVE) и в VenueSimulator (SIM). Оба ExecutionReport публикуются в `execution.venue`. Divergence Service сравнивает пары по `clientOrderId`; дашборд показывает delta fillRate / delta slippage / delta latency.

3. **Тестирование новых моделей impact/latency:**
   Инженер создаёт SimSession с изменёнными параметрами ImpactModel (LINEAR → SQRT), симулирует исполнения на живых данных без воздействия на боевые позиции, анализирует результаты в ClickHouse.

4. **Симуляция деградации площадки:**
   Оператор конфигурирует RejectionModel с `randomRejectionRate=0.15`, проверяя корректность обработки ошибок F-12 (REJECTED → fallback → retry) без реального воздействия на биржу.

5. **Переход SIM → LIVE (go-live):**
   После успешной валидации оператор переключает `routingMode=LIVE_ONLY` через Admin API. VenueSimRouter атомарно переключается; следующие ChildOrderRequest уходят напрямую в EVC.

6. **Калибровка моделей по SHADOW-данным:**
   При накоплении SHADOW-статистики (расхождение SIM vs LIVE > threshold) оператор обновляет параметры LatencyModel/ImpactModel через hot reload без перезапуска VenueSimulator.

### 1.4. Цели и ограничения

**Цели:**
- Обеспечить реалистичную симуляцию исполнения на живых рыночных данных без реального выхода на внешние площадки.
- Поддержать постепенный переход от симуляции к боевому режиму с минимальным риском.
- Обеспечить инструментарий для онлайн-калибровки моделей impact и latency по SHADOW-данным.
- Реализовать полный аудит-трейл каждого симулированного исполнения с привязкой к ID исходного LOB.

**Ограничения:**
- Точность симуляции ограничена частотой обновления VenueSnapshot; при высоком `lobAge` симуляция менее точна.
- VenueSimulator не воспроизводит эффекты HFT-уровня (front-running, latency arbitrage на микросекундах).
- LOB-depletion mode корректен только при серийных ордерах по одному инструменту.
- SimExecutionReport не является юридически значимым исполнением и не влияет на реальный P&L провайдера.

---

## 2. BRD — Бизнес-требования

### 2.1. Требования и ограничения

**Функциональные требования:**

- Система ДОЛЖНА принимать живые VenueSnapshot от F-11 (`venue.snapshots`) и использовать их как источник ликвидности для VenueSimulator без изменения боевого пайплайна F-11.
- Система ДОЛЖНА реализовать VenueSimRouter с тремя режимами: `SIM_ONLY`, `LIVE_ONLY`, `SHADOW`.
- Система ДОЛЖНА обеспечить переключение routingMode без прерывания потока ExecutionIntent/child-ордеров.
- VenueSimulator ДОЛЖЕН поддерживать: LatencyModel (LOGNORMAL, UNIFORM, FIXED, EMPIRICAL), ImpactModel (LINEAR, SQRT, POWER_LAW, LEVEL_BY_LEVEL), FeeModel (maker/taker/gas), RejectionModel.
- Система ДОЛЖНА генерировать SimExecutionReport со всеми стандартными полями ExecutionReport (F-12) плюс расширенными полями (`simMode`, `simSessionId`, `lobSnapshotId`, `lobAge`, `impactBps`, `slippageBps`, `latencySampleMs`).
- Система ДОЛЖНА публиковать SimExecutionReport в Kafka `execution.venue` с маркером `simMode=true`; опционально дублировать в `sim.execution.venue`.
- Система ДОЛЖНА обеспечить изоляцию sim-позиций в Ledger: SimExecutionReport с `simMode=true` обновляет отдельную sim-книгу, не влияя на боевые позиции.
- Система ДОЛЖНА хранить историю всех SimExecutionReport в ClickHouse со всеми полями симуляции.
- Система ДОЛЖНА поддерживать управление SimSession через Admin API: создание, активация, пауза, hot reload моделей, завершение.
- В режиме SHADOW система ДОЛЖНА записывать divergence-метрики (SIM vs LIVE) по каждому `hedgeFlowId`: delta fillRate, delta slippage, delta latency, delta fee.
- Система ДОЛЖНА реализовать stale-LOB защиту: при `lobAge > staleLobThresholdMs` VenueSimulator генерирует `SIM_STALE_LOB` и НЕ рассчитывает исполнение.

**Нефункциональные требования:**

- Latency симуляции (overhead F-20 без учёта LatencyModel sample): p95 < 50 мс.
- Точность LOB-matching: расчёт filledQty/avgPrice с ошибкой ≤ 1% при идентичном LOB.
- Пропускная способность: ≥ 500 симулируемых child-ордеров/сек суммарно.
- Свежесть VenueSnapshot в VenueSimulator: p95 lag < 100 мс от публикации в `venue.snapshots`.
- Хранение SimExecutionReport в ClickHouse: не менее 90 дней.
- Admin API response: p95 < 200 мс.

### 2.2. Диаграмма последовательности (бизнес-уровень)

```mermaid
sequenceDiagram
    participant OP as Оператор
    participant UI as Admin UI
    participant F16 as F-20 (SimRouter + Simulator)
    participant F11 as F-11 (EVC + Normalizer)
    participant VENUE as Внешняя Площадка
    participant F12 as F-12 (Venue Exec Adapter)
    participant LEDGER as Settlement Ledger

    Note over F11,VENUE: Боевой режим — приём живых стаканов
    loop Живой LOB-поток
        VENUE-->>F11: LOB update (WebSocket/REST)
        F11-->>F16: VenueSnapshot → venue.snapshots
        F16->>F16: Обновить LOB-кэш симулятора
    end

    Note over OP,UI: Оператор создаёт SimSession
    OP->>UI: Создать SimSession (SIM_ONLY, BTC/USDT, модели)
    UI->>F16: POST /sim/sessions
    F16-->>UI: simSessionId, status=ACTIVE

    Note over F12,F16: Симуляция исполнения child-ордера
    F12->>F16: ChildOrderRequest (hedgeFlowId, symbol, qty, side)
    F16->>F16: VenueSimRouter: routingMode=SIM_ONLY → VenueSimulator
    F16->>F16: Загрузить актуальный VenueSnapshot + проверить lobAge
    F16->>F16: LEVEL_BY_LEVEL matching + ImpactModel + FeeModel + RejectionModel
    F16->>F16: LatencyModel → async wait(latencySampleMs)
    F16-->>F12: SimExecutionReport (simMode=true, filledQty, avgPrice, fee, lobSnapshotId)
    F12->>LEDGER: execution.venue (simMode=true)
    LEDGER->>LEDGER: Обновить sim-книгу позиций
```

### 2.3. Детальные сценарии

#### Сценарий 1: SIM_ONLY — Happy Path

1. Оператор создаёт SimSession через Admin UI: `routingMode=SIM_ONLY`, `scopeVenues=["binance_sim"]`, `scopeInstruments=["BTCUSDT"]`.
2. SimSession сохраняется в PostgreSQL; событие активации публикуется в Kafka `sim.config`.
3. VenueSimRouter загружает конфигурацию, переключает маршрутизацию для `binance/BTCUSDT`.
4. F-11 продолжает получать живой LOB от реальной Binance → публикует VenueSnapshot в `venue.snapshots`.
5. VenueSimulator обновляет LOB-кэш.
6. F-12 генерирует ExecutionIntent → Venue Execution Adapter создаёт ChildOrderRequest.
7. VenueSimRouter перехватывает запрос, определяет `simMode=SIM_ONLY` → VenueSimulator.
8. VenueSimulator: проверяет `lobAge < staleLobThresholdMs`.
9. LEVEL_BY_LEVEL: последовательно поглощает уровни стакана до запрошенного qty или исчерпания ликвидности.
10. ImpactModel: рассчитывает сдвиг avgPrice.
11. FeeModel: рассчитывает fee.
12. RejectionModel: проверяет условия отказа.
13. LatencyModel: сэмплирует задержку (lognormal p50=35мс, p95=90мс).
14. Через `latencySampleMs` публикует SimExecutionReport в `execution.venue` + `sim.execution.venue`.
15. Settlement Ledger: видит `simMode=true`, обновляет sim-книгу.
16. ClickHouse: сохраняет SimExecutionReport в `sim_execution_reports`.

#### Сценарий 2: SHADOW — сравнение SIM vs LIVE

1. SimSession с `routingMode=SHADOW` активирована.
2. VenueSimRouter получает ChildOrderRequest.
3. Fork (параллельно):
   - **LIVE fork**: → External Venues Connector → реальная Binance → ExecutionReport (`simMode=false`).
   - **SIM fork**: → VenueSimulator → SimExecutionReport (`simMode=true`).
4. Оба отчёта публикуются в `execution.venue`.
5. Divergence Service сопоставляет пары по `clientOrderId`: delta fillRate, delta avgPrice, delta latency → `sim_divergence_log`.
6. При `divergence > threshold` — алерт оператору.

#### Сценарий 3: Stale LOB

1. Соединение F-11 с биржей прервано; VenueSnapshot перестал обновляться.
2. VenueSimulator: `lobAge = now - snapshot.timestamp > staleLobThresholdMs`.
3. SimExecutionReport: `status=REJECTED`, `rejectReason=SIM_STALE_LOB`.
4. Алерт в `sim.alerts`: `STALE_LOB_DETECTED`.
5. F-12 обрабатывает REJECTED по стандартной логике fallback.

#### Сценарий 4: SIM → LIVE переключение

1. Оператор: `PATCH /sim/sessions/{id}` с `routingMode=LIVE_ONLY`.
2. VenueSimRouter атомарно обновляет конфигурацию.
3. Следующие ChildOrderRequest проксируются напрямую в EVC.
4. SimSession → `status=COMPLETED`.

### 2.4. Обработка ошибок

| Сценарий ошибки | Поведение | ExecutionReport | Алерт |
|---|---|---|---|
| `lobAge > staleLobThresholdMs` | Отказ от симуляции | `REJECTED, SIM_STALE_LOB` | `SIM_STALE_LOB` в sim.alerts |
| LOB пуст (нет ликвидности) | Нулевой fill | `REJECTED, SIM_NO_LIQUIDITY` | — |
| RejectionModel: random reject | Симулированный отказ | `REJECTED, SIM_RANDOM_REJECT` | — |
| LatencyModel: timeout | Sim-таймаут | `REJECTED, SIM_TIMEOUT` | При высоком rate |
| SimSession не активна | VenueSimRouter → LIVE | Стандартный LIVE-репорт | Warn-лог |
| Kafka `venue.snapshots` недоступна | NO_LOB режим | Все ордера `SIM_STALE_LOB` | `SIM_LOB_SOURCE_DOWN` |
| Overfill guard (filledQty > targetQty) | Trim до targetQty | `FILLED` с trimmedQty | — |

### 2.5. UX — Admin UI (Sim Manager)

- **SimSession Manager** — список активных/прошедших SimSession; создание, пауза, переключение routingMode, завершение; статусы real-time.
- **Live Sim Feed** — real-time поток SimExecutionReport с полями `lobAge`, `impactBps`, `latencySampleMs`, `slippageBps`; фильтрация по `simSessionId/venue/symbol`.
- **Sim vs Live Comparison** (SHADOW) — боковое сравнение SIM и LIVE исполнения по `hedgeFlowId`: delta fill rate, delta slippage, delta latency.
- **LOB Quality Monitor** — график `lobAge` по venue/symbol; алерты Stale LOB.
- **Impact Analysis** — распределение `impactBps` по urgency/qty/venue; сравнение с предсказанием ImpactModel.
- **Model Calibration Panel** — отображение и hot-update параметров LatencyModel/ImpactModel/FeeModel/RejectionModel с мгновенным применением.

### 2.6. Acceptance Criteria

1. **F16-1**: При `routingMode=SIM_ONLY` ни один реальный вызов к EVC для child-ордеров не производится; все ExecutionReport имеют `simMode=true`.
2. **F16-2**: При `routingMode=SHADOW` каждый ChildOrderRequest порождает ровно два ExecutionReport: `simMode=false` (LIVE) и `simMode=true` (SIM).
3. **F16-3**: VenueSimulator использует актуальный VenueSnapshot из `venue.snapshots`; `lobSnapshotId` ссылается на конкретный снапшот.
4. **F16-4**: При `lobAge > staleLobThresholdMs` SimExecutionReport содержит `status=REJECTED, rejectReason=SIM_STALE_LOB`; реальных ордеров на биржу не уходит.
5. **F16-5**: SimExecutionReport с `simMode=true` не изменяет реальные позиции провайдера в Ledger.
6. **F16-6**: Все SimExecutionReport сохраняются в ClickHouse в течение ≤ 1 сек после публикации.
7. **F16-7**: Переключение `routingMode` через Admin API применяется в течение ≤ 500 мс.
8. **F16-8**: При идентичном LOB расчёт filledQty совпадает с ожидаемым LEVEL_BY_LEVEL с точностью ≤ 1%.
9. **F16-9**: Latency overhead F-20 (без LatencyModel sample): p95 < 50 мс.
10. **F16-10**: Divergence-метрики (SHADOW) корректно записываются в `sim_divergence_log` по каждому `clientOrderId`.
11. **F16-11**: Hot update параметров через Admin API применяется к следующим запросам без перезапуска VenueSimulator.
12. **F16-12**: В режиме LIVE_ONLY overhead роутера ≤ 5 мс.

---

## 3. Требования к качеству

### 3.1. SLO и метрики

| Метрика | SLO | Комментарий |
|---|---|---|
| Latency F-20 overhead (без LatencyModel) | p95 < 50 мс | Время работы VenueSimulator |
| LOB freshness в SimCache | p95 lag < 100 мс | Задержка venue.snapshots → кэш |
| Точность LOB-matching (объём) | ≤ 1% ошибка | При идентичном снапшоте |
| Точность цены (avgPrice) | ≤ 0.5 bps | LEVEL_BY_LEVEL mode |
| Пропускная способность | ≥ 500 child-ордеров/сек | Суммарно по всем SimSession |
| Stale LOB обнаружение | ≤ 200 мс | От превышения порога до первого reject |
| Admin API response | p95 < 200 мс | CRUD SimSession |
| SIM→LIVE переключение | ≤ 500 мс | От API-ответа до применения |
| SHADOW divergence lag | ≤ 1 сек | От получения обоих отчётов до записи |
| ClickHouse sim-запись | ≤ 1 сек | От публикации до сохранения |

### 3.2. Матрица компонентов

| Компонент | Режим работы | Роль в F-20 |
|---|---|---|
| External Venues Connector (F-11) | Боевой (без изменений) | Получение живого LOB |
| Venue Market Data Normalizer (F-11) | Боевой (без изменений) | Публикация VenueSnapshot |
| VenueSimRouter (F-20, новый) | Симуляция | Маршрутизация SIM/LIVE/SHADOW |
| VenueSimulator (F-20, новый) | Симуляция | LOB-matching, impact, latency, fee, rejection |
| SimSession Manager (F-20, новый) | Симуляция | CRUD SimSession, hot reload |
| Divergence Service (F-20, новый) | SHADOW-анализ | SIM vs LIVE delta metrics |
| Venue Execution Adapter (F-12) | Без изменений | Источник ChildOrderRequest |
| Settlement Ledger | Расширен: sim-книга | Изолированные sim-позиции |
| Risk Manager | Расширен | Анализ sim-исполнений |
| ClickHouse | Расширен | sim_execution_reports, sim_divergence_log |
| PostgreSQL | Расширен | sim_sessions, sim_child_orders |
| Admin UI | Расширен | Sim Manager, калибровка моделей |

---

## 4. Техническая архитектура (TRD)

### 4.1. Состав компонентов

- **VenueSimRouter** — singleton-сервис, хранящий карту `venueId+symbol → routingMode`. Слушает `sim.config`. Реализует fork-логику для SHADOW-режима.
- **VenueSimulator** — stateful-сервис с LOB-кэшем. Подписан на `venue.snapshots`. Реализует matching, impact, latency (async delay), fee, rejection.
- **SimSession Manager** — управление жизненным циклом SimSession. PostgreSQL + Kafka `sim.config`.
- **Divergence Service** — SHADOW-режим: соединяет LIVE и SIM по `clientOrderId`, рассчитывает дельты, пишет в ClickHouse.

### 4.2. Диаграмма последовательности (техническая)

```mermaid
sequenceDiagram
    participant VENUE as External Venue
    participant EVC as External Venues Connector (F-11)
    participant NORM as Venue Market Data Normalizer (F-11)
    participant K_VS as Kafka venue.snapshots
    participant SIM as VenueSimulator (F-20)
    participant ROUTER as VenueSimRouter (F-20)
    participant VEA as Venue Execution Adapter (F-12)
    participant K_SIM as Kafka sim.config
    participant PG as PostgreSQL sim_sessions
    participant K_EV as Kafka execution.venue
    participant K_SEV as Kafka sim.execution.venue
    participant LEDGER as Settlement Ledger
    participant CH as ClickHouse
    participant DIV as Divergence Service (F-20)
    participant ADMIN as Admin UI / API

    loop Живой LOB-поток (боевой режим)
        VENUE-->>EVC: LOB update (WebSocket)
        EVC->>NORM: Raw market data
        NORM->>K_VS: VenueSnapshot{snapshotId, venueId, symbol, biddepth, askdepth,...}
        K_VS-->>SIM: VenueSnapshot
        SIM->>SIM: Обновить LOB-кэш[venueId][symbol]
    end

    ADMIN->>PG: INSERT sim_sessions (routingMode=SIM_ONLY, scope, models)
    PG-->>ADMIN: simSessionId
    ADMIN->>K_SIM: SimConfigEvent{simSessionId, routingMode, scope, models}
    K_SIM-->>ROUTER: Применить конфигурацию

    VEA->>ROUTER: ChildOrderRequest{childOrderId, hedgeFlowId, venueId, symbol, qty, side, orderType, simSessionId}

    alt routingMode = SIM_ONLY
        ROUTER->>SIM: SimulateOrder(ChildOrderRequest)
        SIM->>SIM: 1. Загрузить LOB-кэш[venueId][symbol]
        SIM->>SIM: 2. Проверить lobAge < staleLobThresholdMs
        SIM->>SIM: 3. LEVEL_BY_LEVEL matching → filledQty, levels
        SIM->>SIM: 4. ImpactModel → avgPrice = f(qty, LOB, impactCoeff)
        SIM->>SIM: 5. FeeModel → fee = filledQty * avgPrice * feeRate
        SIM->>SIM: 6. RejectionModel → проверка условий отказа
        SIM->>SIM: 7. LatencyModel → latencySample ~ Lognormal(p50, p95)
        SIM-->>SIM: async wait(latencySampleMs)
        SIM->>K_EV: SimExecutionReport{simMode=true, filledQty, avgPrice, fee, lobSnapshotId, lobAge, impactBps, latencySampleMs}
        SIM->>K_SEV: SimExecutionReport (дубль)
        K_EV-->>LEDGER: SimExecutionReport
        LEDGER->>LEDGER: simMode=true → обновить sim-книгу
        K_EV-->>CH: sim_execution_reports INSERT

    else routingMode = SHADOW
        par LIVE fork
            ROUTER->>EVC: PlaceChildOrder(ChildOrderRequest)
            EVC->>VENUE: REST/WS order
            VENUE-->>EVC: ExecutionReport (LIVE)
            EVC->>K_EV: ExecutionReport{simMode=false}
        and SIM fork
            ROUTER->>SIM: SimulateOrder(ChildOrderRequest)
            SIM->>K_EV: SimExecutionReport{simMode=true}
        end
        K_EV-->>DIV: Оба отчёта (по clientOrderId)
        DIV->>DIV: delta fillRate, delta price, delta latency
        DIV->>CH: sim_divergence_log INSERT
    end
```

### 4.3. Матрица взаимодействий компонентов F-20

| Источник | Получатель | Протокол | Payload | FID |
|---|---|---|---|---|
| Kafka `venue.snapshots` | VenueSimulator | Kafka consume | VenueSnapshot | F16-SIM-1 |
| Venue Execution Adapter | VenueSimRouter | gRPC/in-process | ChildOrderRequest | F16-SIM-2 |
| VenueSimRouter | VenueSimulator | in-process | ChildOrderRequest + simSessionId | F16-SIM-3 |
| VenueSimRouter | External Venues Connector | gRPC (LIVE/SHADOW) | PlaceChildOrder | F16-SIM-4 |
| VenueSimulator | Kafka `execution.venue` | Kafka produce | SimExecutionReport | F16-SIM-5 |
| VenueSimulator | Kafka `sim.execution.venue` | Kafka produce | SimExecutionReport | F16-SIM-6 |
| VenueSimulator | Kafka `sim.alerts` | Kafka produce | SimAlert | F16-SIM-7 |
| Admin UI / API | SimSession Manager | REST HTTP | SimSession CRUD | F16-SIM-8 |
| SimSession Manager | PostgreSQL | SQL | sim_sessions CRUD | F16-SIM-9 |
| SimSession Manager | Kafka `sim.config` | Kafka produce | SimConfigEvent | F16-SIM-10 |
| Kafka `sim.config` | VenueSimRouter | Kafka consume | SimConfigEvent | F16-SIM-11 |
| Kafka `execution.venue` | Divergence Service | Kafka consume | ExecutionReport (LIVE+SIM) | F16-SIM-12 |
| Divergence Service | ClickHouse | HTTP insert | sim_divergence_log | F16-SIM-13 |
| Kafka `execution.venue` | ClickHouse | Kafka→CH connector | sim_execution_reports | F16-SIM-14 |
| Kafka `execution.venue` | Settlement Ledger | Kafka consume | SimExecutionReport (sim-book) | F16-SIM-15 |

### 4.4. Алгоритм LOB-matching в VenueSimulator

При получении ChildOrderRequest с `qty`, `side`, `orderType`:

1. Загрузить LOB-снапшот для `venueId+symbol`; проверить `lobAge < staleLobThresholdMs`.
2. Выбрать сторону стакана: BUY → `askdepth` (ASC по цене); SELL → `biddepth` (DESC по цене).
3. При `orderType=LIMIT` отфильтровать уровни хуже лимитной цены.
4. LEVEL_BY_LEVEL matching: поглощать уровни до `qty` или исчерпания ликвидности.
5. Рассчитать VWAP:

$$\text{avgPrice} = \frac{\sum_i \text{fills}[i].\text{price} \cdot \text{fills}[i].\text{qty}}{\text{filledQty}}$$

6. Применить ImpactModel. LINEAR: $\Delta p = \alpha \cdot \text{filledQty}$; SQRT: $\Delta p = \alpha \cdot \sqrt{\text{filledQty}}$.
7. Итоговая цена с impact: $\text{avgPrice}_{\text{impact}} = \text{avgPrice} \pm \Delta p$.
8. Рассчитать:

$$\text{impactBps} = \frac{|\text{avgPrice}_{\text{impact}} - \text{mid}|}{\text{mid}} \times 10000$$

9. Применить FeeModel: `fee = filledQty × avgPrice_impact × feeRate`.
10. Проверить RejectionModel; если `filledQty == 0 && insufficientLiquidityEnabled` → `REJECTED`.
11. Сэмплировать `latencySample ~ LatencyModel.distribution(p50, p95, p99)`.
12. Сформировать SimExecutionReport.

### 4.5. Контракты данных

#### ChildOrderRequest (Kafka / gRPC)

```json
{
  "childOrderId": "uuid",
  "hedgeFlowId": "uuid",
  "venueId": "binance",
  "symbol": "BTCUSDT",
  "side": "SELL",
  "orderType": "LIMIT",
  "qty": 0.5,
  "price": 68000.0,
  "timeInForce": "GTC",
  "clientOrderId": "hf-abc123-001",
  "simSessionId": "uuid-or-null"
}
```

#### SimExecutionReport (Kafka `execution.venue`)

```json
{
  "executionId": "uuid",
  "venueId": "binance_sim",
  "symbol": "BTCUSDT",
  "side": "SELL",
  "filledQty": 0.5,
  "avgPrice": 67982.15,
  "fee": 6.798,
  "status": "FILLED",
  "timestamp": "2026-04-30T10:00:00.125Z",
  "clientOrderId": "hf-abc123-001",
  "hedgeFlowId": "uuid",
  "simMode": true,
  "simSessionId": "uuid",
  "lobSnapshotId": "uuid",
  "lobAge": 42,
  "impactBps": 2.8,
  "slippageBps": 3.1,
  "latencySampleMs": 68
}
```

#### PostgreSQL `sim_sessions`

```sql
CREATE TABLE sim_sessions (
    sim_session_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name               VARCHAR(255) NOT NULL,
    routing_mode       VARCHAR(20)  NOT NULL CHECK (routing_mode IN ('SIM_ONLY','LIVE_ONLY','SHADOW')),
    scope_venues       TEXT[]       NOT NULL,
    scope_instruments  TEXT[]       NOT NULL,
    latency_model      JSONB        NOT NULL,
    impact_model       JSONB        NOT NULL,
    fee_model          JSONB        NOT NULL,
    rejection_model    JSONB        NOT NULL,
    stale_lob_threshold_ms INTEGER   NOT NULL DEFAULT 2000,
    partial_fill_mode  VARCHAR(20)  NOT NULL DEFAULT 'LEVEL_BY_LEVEL',
    status             VARCHAR(20)  NOT NULL DEFAULT 'ACTIVE'
                       CHECK (status IN ('ACTIVE','PAUSED','COMPLETED','CANCELLED')),
    created_at         TIMESTAMPTZ  NOT NULL DEFAULT now(),
    activated_at       TIMESTAMPTZ,
    completed_at       TIMESTAMPTZ,
    created_by         VARCHAR(255) NOT NULL
);
```

#### ClickHouse `sim_execution_reports`

```sql
CREATE TABLE sim_execution_reports (
    execution_id       UUID,
    sim_session_id     UUID,
    hedge_flow_id      UUID,
    child_order_id     UUID,
    venue_id           String,
    symbol             String,
    side               String,
    filled_qty         Float64,
    avg_price          Float64,
    fee                Float64,
    status             String,
    sim_mode           UInt8,
    lob_snapshot_id    UUID,
    lob_age_ms         UInt32,
    impact_bps         Float64,
    slippage_bps       Float64,
    latency_sample_ms  UInt32,
    timestamp          DateTime64(3),
    created_at         DateTime64(3) DEFAULT now()
) ENGINE = MergeTree()
PARTITION BY toYYYYMM(timestamp)
ORDER BY (sim_session_id, timestamp, venue_id, symbol);
```

#### ClickHouse `sim_divergence_log` (SHADOW mode)

```sql
CREATE TABLE sim_divergence_log (
    divergence_id      UUID,
    sim_session_id     UUID,
    client_order_id    String,
    hedge_flow_id      UUID,
    venue_id           String,
    symbol             String,
    live_filled_qty    Float64,
    sim_filled_qty     Float64,
    delta_fill_qty     Float64,
    live_avg_price     Float64,
    sim_avg_price      Float64,
    delta_price_bps    Float64,
    live_latency_ms    UInt32,
    sim_latency_ms     UInt32,
    delta_latency_ms   Int32,
    live_fee           Float64,
    sim_fee            Float64,
    delta_fee          Float64,
    timestamp          DateTime64(3)
) ENGINE = MergeTree()
PARTITION BY toYYYYMM(timestamp)
ORDER BY (sim_session_id, timestamp);
```

---

## 5. Нагрузочное тестирование

### 5.1. SLO и целевые показатели

| Показатель | Цель |
|---|---|
| Throughput VenueSimulator | >= 500 child-ордеров/сек |
| Latency F-20 overhead | p95 < 50 мс, p99 < 100 мс |
| LOB-cache freshness | p95 lag < 100 мс |
| ClickHouse ingest | <= 1 сек до записи |
| SHADOW fork overhead | p95 < 10 мс доп. задержки |

### 5.2. Нагрузочные тесты

| Тест | Сценарий | Критерий прохождения |
|---|---|---|
| LT-1 | 500 child-ордеров/сек, SIM_ONLY, 5 мин | p95 latency < 50 мс, нет ошибок Kafka publish |
| LT-2 | 200 child-ордеров/сек, SHADOW, 5 мин | Оба отчёта доставлены; LIVE delta overhead p95 < 10 мс |
| LT-3 | LOB freeze на 5 сек | Все ордера получают SIM_STALE_LOB в <= 200 мс |
| LT-4 | Hot reload при нагрузке 300/сек | Применение <= 500 мс; нет потери ордеров |

### 5.3. Latency-таблица по этапам

| Этап | p50 | p95 | p99 |
|---|---|---|---|
| VenueSnapshot → LOB-кэш | 15 мс | 50 мс | 80 мс |
| VenueSimRouter routing decision | < 1 мс | 2 мс | 5 мс |
| LOB-matching + ImpactModel | 2 мс | 8 мс | 15 мс |
| FeeModel + RejectionModel | < 1 мс | 1 мс | 2 мс |
| Kafka publish SimExecutionReport | 3 мс | 10 мс | 20 мс |
| **Итого F-20 overhead** | **~6 мс** | **~21 мс** | **~42 мс** |
| + LatencyModel sample (p50=35мс) | 35 мс | 90 мс | 150 мс |

---

## 6. Definition of Done

Чек-лист DoD для F-20 «Симулятор исполнения на внешних площадках (Live Venue Simulator)» (для Jira):

1. [ ] Реализован **VenueSimulator**: подписан на Kafka `venue.snapshots`, поддерживает актуальный LOB-кэш по `venueId+symbol`, реализует LEVEL_BY_LEVEL matching, ImpactModel (LINEAR/SQRT/POWER_LAW), FeeModel, RejectionModel, LatencyModel с async delay.
2. [ ] Реализован **VenueSimRouter**: маршрутизирует ChildOrderRequest в SIM_ONLY/LIVE_ONLY/SHADOW; поддерживает hot reload конфигурации из Kafka `sim.config` без перезапуска.
3. [ ] Реализован **SimSession Manager**: CRUD SimSession через Admin API, сохранение в PostgreSQL `sim_sessions`, публикация изменений в `sim.config`.
4. [ ] Реализован алгоритм LOB-matching: расчёт `filledQty`, `avgPrice`, `impactBps` соответствует спецификации TRD; при идентичном LOB ошибка объёма <= 1%.
5. [ ] Реализована stale-LOB защита: при `lobAge > staleLobThresholdMs` VenueSimulator генерирует `REJECTED, SIM_STALE_LOB`; алерт в `sim.alerts`.
6. [ ] SimExecutionReport публикуется в `execution.venue` со всеми полями F-12 плюс расширенными полями симуляции (`simMode=true`, `simSessionId`, `lobSnapshotId`, `lobAge`, `impactBps`, `slippageBps`, `latencySampleMs`); дублируется в `sim.execution.venue`.
7. [ ] Settlement Ledger корректно обрабатывает `simMode=true`: обновляет изолированную sim-книгу, не изменяя боевые позиции.
8. [ ] ClickHouse принимает SimExecutionReport в таблицу `sim_execution_reports` в течение <= 1 сек; все поля симуляции присутствуют.
9. [ ] Реализован **Divergence Service** (SHADOW): сопоставление LIVE и SIM по `clientOrderId`, расчёт delta-метрик, запись в `sim_divergence_log`.
10. [ ] Пройден набор юнит-тестов VenueSimulator (U1–U10): happy-path FILLED, partial fill, stale LOB, LEVEL_BY_LEVEL precision, ImpactModel LINEAR/SQRT, FeeModel maker/taker, RejectionModel random/price_constraint, LatencyModel sampling, SHADOW fork isolation, overfill guard.
11. [ ] Пройден набор интеграционных тестов (IT-1–IT-5): SIM_ONLY полный цикл, SHADOW dual-report + divergence logging, stale LOB + alert, hot reload при нагрузке, SIM→LIVE атомарное переключение.
12. [ ] Пройден SLA-тест: p95 latency F-20 overhead < 50 мс при >= 500 child-ордеров/сек; задокументированы p50/p95/p99 по этапам (таблица TRD 5.3).
13. [ ] Пройден нагрузочный тест LT-1 (500/сек, SIM_ONLY, 5 мин): нет ошибок Kafka publish, нет потери SimExecutionReport.
14. [ ] В Admin UI реализован **SimSession Manager**: список сессий, создание, пауза, переключение routingMode, завершение, статусы real-time.
15. [ ] В Admin UI реализован **Live Sim Feed**: real-time поток SimExecutionReport с `lobAge`, `impactBps`, `latencySampleMs`, `slippageBps`; фильтрация по `simSessionId/venue/symbol`.
16. [ ] В Admin UI реализован **Sim vs Live Comparison** (SHADOW): delta fillRate / delta price / delta latency по `hedgeFlowId`; алерт при `divergence > threshold`.
17. [ ] Настроены метрики и алерты: LOB freshness, rejection rate по типу, latency percentiles VenueSimulator, SHADOW divergence rate, ошибки hot reload.
18. [ ] Существует операторский runbook: массовые SIM_STALE_LOB (диагностика F-11), высокое SHADOW divergence (перекалибровка моделей), деградация throughput (scaling), SIM→LIVE go-live процедура.
19. [ ] Обновлена архитектурная документация: схемы потоков с VenueSimRouter/VenueSimulator, контракты новых Kafka-топиков (`sim.config`, `sim.alerts`, `sim.execution.venue`), схемы PostgreSQL/ClickHouse, интеграции с F-11, F-12, F-15.
