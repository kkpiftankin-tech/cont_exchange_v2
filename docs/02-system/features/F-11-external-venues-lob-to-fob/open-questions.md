# Открытые вопросы — F-11

1. **Источник execution reports для L3-калибровки.** Спецификация говорит «калибровать L3 по фактическим исполнениям», но не уточняет:
   - какой топик читать: `execution.venue` (новый F-11 поток) или `execution.reports` (legacy)?
   - какое окно ретроспективы (минуты/часы)?
   - как взвешивать reports от разных площадок (только same-venue или cross-venue)?
   - как защищаться от self-influence (наш собственный execution смещает калибровку)?

2. **L3 weighted mix между моделью и наблюдениями.** В коде есть формула $S_{L3} = (1-w)\cdot S_{\text{model}} + w\cdot S_{\text{execution}}$ ([liquidity_curve_producer.hpp:30](../../../../cpp/venues/src/app/liquidity_curve_producer.hpp#L30)), но политика выбора $w$ — статичная константа vs адаптивная (по `confidence`/`epsilon3`)?

3. **Routing policy (allow / caution / avoid / block).** Сейчас матрица решений жёстко зашита в [main.cpp](../../../../cpp/venues/src/main.cpp#L170-L184) и дублируется в Execution Planning. Должна ли политика лежать в `venue_config` (per-venue override) или в централизованном `routing_policy` (новая таблица в PostgreSQL)?

4. **Slippage thresholds per venue.** В DoD говорится о quality-метриках, но конкретные пороги (max acceptable slippage для allow vs avoid vs block) не зафиксированы. Должны быть per-venue (BTC/USDT на Binance иной диапазон, чем WBTC/USDC на Uniswap)?

5. **synthetic_orders shape mismatch.** Поля таблицы в IN-004 (pl, ph, qrate, qmax, curveid, snapshotid, status) частично не совпадают с тем, что использует [postgres_synthetic_order_repository.cpp](../../../../cpp/venues/src/infra/postgres_synthetic_order_repository.cpp). Перед T-F11-100 нужно согласовать DDL — взять схему из спецификации, адаптировать код, либо наоборот.

6. **DEX/AMM конкретные quirks.** Текущий код знает только Uniswap v3 (concentrated liquidity). Открыто:
   - Curve / Balancer (StableSwap, weighted pools) — нужны отдельные `_pool_extractor`?
   - Permit2/EIP-2612 для on-chain execution?
   - MEV-protected execution (Flashbots, private mempool) — где должна быть логика?

7. **Fallback policy при OPEN circuit breaker.** Spec говорит «fallback на другие venues». Кто принимает решение о fallback:
   - Execution Planning (F-12)?
   - Venue Health & Routing (текущий cpp/venue_health)?
   - Risk Manager как kill-switch (F-08, F-16)?
   - Документально — Execution Planning, но никто не реализовал перенаправление intents.

8. **`marketdata.raw` deprecation.** Sec C-2 — два топика дублируются. Сроки миграции consumers F-05? Какой формат payload остаётся source-of-truth (MarketDataRaw оборачивает VenueSnapshot или наоборот)?

9. **Backtest determinism.** В `VenueLiquidityCurve` есть `schema_version`, `producer_version`. Но как ловить bit-level drift между production и backtest при идентичных входах? Нужны golden fixtures и diff-utility — открыто, где они живут (Testing/golden-fixtures/?).

10. **Rate limits для CEX.** Binance/Coinbase имеют per-API-key квоты. Где хранятся секреты в проде (HashiCorp Vault? AWS Secrets Manager?)? Текущая реализация — env-vars, что неприемлемо для prod (см. §22 CLAUDE.md).

11. **PII в venue.health.** Содержит ли `reason` поле PII или внутренние секреты (API keys в стектрейсе)? Нужно политику scrub-before-publish.

12. **Множественные SyntheticFlowOrder для одного curve.** На каждый снапшот может строиться несколько Synthetic (per side, per bucket). Lifecycle (`active` → `expired` → `used`) описан в DDL, но в коде нет TTL-cleanup'а — открыто, где он живёт.

13. **Coexistence с Market Maker Curves (F-10).** F-10 публикует свои собственные CSLO от наших MM-агентов; F-11 публикует кривые от внешних venue. Должны ли они смешиваться в одном топике (`venue.liquidity.fob` vs `mm.curves`) или жить отдельно? Сейчас отдельно, но F-04 solver видит оба.

14. **Cross-venue arbitrage detection.** Если у нас есть VenueLiquidityCurve по Binance и Uniswap для BTCUSDT/WBTCUSDC, кто детектирует и реагирует на ценовой разрыв? F-12 Execution Planning или отдельный agent?
