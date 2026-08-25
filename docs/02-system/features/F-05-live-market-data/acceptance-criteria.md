# F-05. Live Market Data — Acceptance Criteria

## Feature

[F-05 feature.yaml](feature.yaml)

## Functional Requirements

| ID | Требование |
|----|-----------|
| F5-1 | Market Data Service ДОЛЖЕН рассчитывать mid-price по формуле `mid = (bestBid + bestAsk) / 2` после каждого BatchResult. |
| F5-2 | Market Data Service ДОЛЖЕН рассчитывать spread = bestAsk − bestBid и spreadBps = spread / mid × 10000. |
| F5-3 | bestBid и bestAsk ДОЛЖНЫ определяться из агрегированных кривых спроса/предложения (findDetachPrice). |
| F5-4 | Market Data Service ДОЛЖЕН рассчитывать effectiveSpread = 2 × \|execPrice − mid\| для каждого FillEvent и сохранять в ClickHouse. |
| F5-5 | Market Data Service ДОЛЖЕН агрегировать volume24h как сумму execQty из fills за скользящее окно 86400 секунд. |
| F5-6 | Market Data Service ДОЛЖЕН формировать bidDepth и askDepth по depthLevels уровням из marketdata_config. |
| F5-7 | Market Data Service ДОЛЖЕН публиковать MarketDataSnapshot в Kafka `marketdata.snapshots` после каждого пересчёта. |
| F5-8 | Market Data Service ДОЛЖЕН транслировать MarketDataUpdate через WebSocket `ws://api/market` в течение 200 ms после BatchResult. |
| F5-9 | Market Data Service ДОЛЖЕН предоставлять REST endpoint GET `/api/v1/marketdata/{asset}`. |
| F5-10 | Market Data Service ДОЛЖЕН предоставлять REST endpoint GET `/api/v1/marketdata/{asset}/history`. |
| F5-11 | Market Data Service ДОЛЖЕН предоставлять gRPC `GetReferencePrices(assets[], tsBatch)` для Matching Backend. |
| F5-12 | При отсутствии внутренних данных Market Data Service ДОЛЖЕН использовать внешние котировки из `marketdata.raw`, устанавливая source = cex\|dex. |
| F5-13 | Market Data Service ДОЛЖЕН сохранять каждый MarketDataSnapshot в ClickHouse `marketdata_snapshots`. |
| F5-14 | При спреде, превышающем пороговое значение из marketdata_config, ДОЛЖЕН генерироваться alert в Kafka `risk.alerts`. |

## Acceptance Criteria (Definition of Done)

- [ ] `ComputeMarketData` корректно рассчитывает mid, spread, depth, volume из BatchResult.
- [ ] `ComputeEffectiveSpread` корректно рассчитывает effective spread из FillEvent.
- [ ] bestBid/bestAsk определяются из агрегированных кривых спроса/предложения.
- [ ] WebSocket `ws://api/market` доставляет MarketDataUpdate < 200 ms после BatchResult.
- [ ] REST GET `/api/v1/marketdata/{asset}` возвращает snapshot, p95 < 50 ms.
- [ ] `GetReferencePrices()` возвращает корректные ReferencePrice для Matching Backend.
- [ ] Fallback на внешние источники при отсутствии внутренних данных работает.
- [ ] MarketDataSnapshot сохраняется в ClickHouse `marketdata_snapshots`.
- [ ] Effective spread сохраняется в ClickHouse `effective_spreads`.
- [ ] Risk alert генерируется при аномальном спреде.
- [ ] Все 10 unit-тестов (U1–U10) пройдены.
- [ ] Все 8 интеграционных тестов (I1–I8) пройдены.
- [ ] Нагрузочные тесты: p95 WS latency < 200 ms при 5000 подключениях.
- [ ] Kill-switch через `marketdata_config.isActive = false` работает.
- [ ] UI wireframe реализован: mid, spread, depth, volume, effective spread, batch info.

## Performance SLA

| Метрика | SLA |
|---------|-----|
| WS publish после BatchResult | p95 < 200 ms |
| REST GET snapshot | p95 < 50 ms |
| WS concurrent subscribers | 5000 на инструмент |
| Effective spread computation | 10 000 fills/sec |
| ClickHouse write throughput | 1 000 snapshots/sec |
