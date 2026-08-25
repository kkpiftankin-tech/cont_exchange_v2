# Frontend API

Документ описывает HTTP API, которое текущий фронтенд (`frontend/web`) использует для авторизации, торговых заявок, истории, графика и батчей.

## 1. Базовая конфигурация

- Базовый префикс API: `/api`
- Таймаут запросов: `REACT_APP_API_TIMEOUT` (по умолчанию `10000` мс)
- Для F-11 UI фронтенд использует только HTTP/REST-контракт. Если backend-сервисы внутри системы общаются по gRPC, это скрыто за API gateway / mock API и напрямую из браузера не вызывается.
- Для разных доменов/сервисов фронтенд использует:
  - `REACT_APP_API_BASE_URL` (auth + batches)
  - `REACT_APP_MARKET_API_BASE_URL` (заявки, баланс, транзакции)
  - `REACT_APP_MATCHING_ENGINE_API_BASE_URL` (клиринговая цена и кривые)

## 2. Аутентификация

### POST `/api/auth/register`
Регистрация пользователя.

Request body:
```json
{
  "email": "user@example.com",
  "login": "user@example.com",
  "password": "password123"
}
```

Response `200`:
```json
{
  "token": "uuid-token"
}
```

### POST `/api/auth/login`
Вход пользователя.

Request body:
```json
{
  "login": "user@example.com",
  "password": "password123"
}
```

Response `200`:
```json
{
  "token": "uuid-token"
}
```

### POST `/api/auth/validate-token`
Проверка токена.

Request body:
```json
{
  "token": "uuid-token"
}
```

Response `200`:
```json
{
  "is-valid": true
}
```

## 3. Авторизационный заголовок для market API

Для запросов в market API фронтенд автоматически добавляет заголовок:

```http
api_key: <token>
```

Токен берется из cookie `token` (для Safari fallback в `localStorage`).

## 4. Баланс, транзакции и заявки

### GET `/api/account/balance`
Получение баланса пользователя.

Response `200`:
```json
[
  { "currency": "USDT", "amount": 10000 },
  { "currency": "BTC", "amount": 2.5 }
]
```

### POST `/api/transactions/deposit`
Создание адреса для пополнения.

Request body:
```json
{
  "currency": "USDT"
}
```

Response `200`:
```json
{
  "address": "demo-usdt-1740000000"
}
```

### POST `/api/transactions/withdraw`
Создание заявки на вывод.

Request body:
```json
{
  "currency": "BTC",
  "amount": 0.1,
  "address": "wallet-address"
}
```

Response `200`:
```json
{
  "ok": true,
  "id": "tx-1740000000"
}
```

### GET `/api/transactions/transfers`
История транзакций (профиль).

Query params (опционально):
- `date_from` (unix seconds)
- `operation` (`deposit` | `withdraw`)
- `status` (например `finished`, `processing`, `pending`, `cancelled`)

Response `200`:
```json
[
  {
    "id": "tx-1001",
    "date": 1740000000,
    "operation": "deposit",
    "currency": "USDT",
    "amount": 5000,
    "status": "finished",
    "address": "demo-usdt-wallet"
  }
]
```

### GET `/api/bids`
История торговых заявок (профиль).

Query params (опционально):
- `date_from` (unix seconds)
- `status` (`pending`, `finished`, `partial`, `cancelled`)

Response `200`:
```json
[
  {
    "id": "bid-2001",
    "create_date": "2026-03-25T10:00:00.000Z",
    "complete_date": null,
    "from_currency": "USDT",
    "to_currency": "BTC",
    "amount_to_buy": 0.35,
    "status": "pending",
    "bought_amount": 0.11,
    "min_price": 64000,
    "max_price": 70000,
    "buy_speed": 0.004,
    "avg_price": 68100
  }
]
```

### POST `/api/bid`
Создание торговой заявки.

Request body:
```json
{
  "from_currency": "USDT",
  "to_currency": "BTC",
  "min_price": 64000,
  "max_price": 70000,
  "amount_to_buy": 0.35,
  "buy_speed": 0.004
}
```

Response `200`:
```json
{
  "id": "bid-1740000000",
  "status": "pending"
}
```

### DELETE `/api/market/:id`
Отмена торговой заявки.

Response `200`:
```json
{
  "ok": true
}
```

## 5. Данные для графика и цены

### GET `/api/clearing-price`
Response `200`:
```json
{
  "price": 68450.3
}
```

### GET `/api/bid-curve`
### GET `/api/ask-curve`

Query params:
- `left_boundary_price`
- `right_boundary_price`

Response `200`:
```json
[
  { "price": 68000.0, "volume": 12000.5 },
  { "price": 68100.0, "volume": 12340.2 }
]
```

## 6. Батчи (список для профиля)

### GET `/api/batches`
Используется на странице профиля для списка батчей.

Response `200`:
```json
{
  "items": [
    {
      "batchId": "batch-2026-03-24-0001",
      "time": "2026-03-24T15:45:12Z",
      "status": "SUCCESS",
      "solveTimeMs": 82,
      "residualNorm": 0.00094
    }
  ],
  "total": 1
}
```

### GET `/api/batches/:id`
Детали батча (сейчас в UI не используется, но endpoint существует).

Response `200`:
```json
{
  "batchId": "batch-2026-03-24-0001",
  "time": "2026-03-24T15:45:12Z",
  "status": "SUCCESS",
  "solveTimeMs": 82,
  "residualNorm": 0.00094,
  "clearPrices": {
    "BTC/USDT": "68250.10"
  },
  "executedRates": {
    "ord-1001": "0.0024"
  },
  "fills": [
    {
      "orderId": "ord-1001",
      "userId": "u-01",
      "instrument": "BTC/USDT",
      "side": "BUY",
      "executedQty": "0.0042",
      "price": "68250.10",
      "executedNotional": "286.65",
      "fee": "0.12"
    }
  ]
}
```

## 7. External Venues

### GET `/api/venues`
Используется на экране `External Venues` для операторского мониторинга площадок и live-обновления статусов.
`frontend-api` проксирует данные из `venues` (`/api/v1/venues*`) через `VENUES_HTTP_ADDR` и автоматически делает fallback на встроенный mock, если `venues` временно недоступен.

Response `200`:
```json
{
  "items": [
    {
      "venueId": "binance",
      "displayName": "Binance Spot",
      "venueType": "cex",
      "symbol": "BTC/USDT",
      "region": "Global",
      "status": "connected",
      "healthScore": 92,
      "latencyMs": 42,
      "errorRate": 0.0042,
      "staleRate": 0.012,
      "fillRate": 0.982,
      "executionQuality": 94,
      "feesBps": 10,
      "tickSize": 0.1,
      "lotSize": 0.0001,
      "bestBid": 68441.5,
      "bestAsk": 68442.7,
      "midPrice": 68442.1,
      "spread": 1.2,
      "volume24h": 1834500000,
      "hedgePnl": 1250.45,
      "hedgeCount": 5,
      "recommendation": "route",
      "updatedAt": "2026-04-06T09:15:00.000Z",
      "dataSource": "venues-api"
    }
  ],
  "total": 5,
  "generatedAt": "2026-04-06T09:15:04.000Z",
  "source": "venues-api",
  "summary": {
    "total": 5,
    "connected": 4,
    "stale": 1,
    "disconnected": 0,
    "empty": 0,
    "healthy": 3,
    "disabled": 0
  }
}
```

### GET `/api/venues/:id`
Возвращает одну площадку по `venueId`.

Response `200`:
```json
{
  "venueId": "binance",
  "displayName": "Binance Spot",
  "venueType": "cex",
  "symbol": "BTC/USDT",
  "region": "Global",
  "status": "connected",
  "healthScore": 92,
  "latencyMs": 42,
  "errorRate": 0.0042,
  "staleRate": 0.012,
  "fillRate": 0.982,
  "executionQuality": 94,
  "feesBps": 10,
  "tickSize": 0.1,
  "lotSize": 0.0001,
  "bestBid": 68441.5,
  "bestAsk": 68442.7,
  "midPrice": 68442.1,
  "spread": 1.2,
  "volume24h": 1834500000,
  "hedgePnl": 1250.45,
  "hedgeCount": 5,
  "recommendation": "route",
  "adminState": "active",
  "routingMode": "auto",
  "config": {
    "venueSymbol": "BTCUSDT",
    "curveLevel": "L2",
    "syntheticEnabled": true,
    "depthLevels": 20,
    "staleThresholdMs": 3000,
    "circuitBreakerEnabled": true,
    "circuitBreakerErrors": 10,
    "circuitBreakerWindowMs": 30000,
    "circuitBreakerCooldownMs": 30000
  },
  "reconnectCount": 2,
  "lastAction": "reconnect",
  "lastActionAt": "2026-04-06T09:15:00.000Z",
  "updatedAt": "2026-04-06T09:15:04.000Z"
}
```

### GET `/api/venues/:id/curves`
Возвращает текущие FOB-кривые внешней ликвидности для detail-экрана площадки.

Response `200`:
```json
{
  "items": [
    {
      "curveId": "binance-buy-curve",
      "venueId": "binance",
      "symbol": "BTC/USDT",
      "side": "buy",
      "level": "L2",
      "tauSec": 30,
      "confidence": 0.92,
      "epsilon1": 0.013,
      "epsilon2": 0.008,
      "epsilon3": 0.006,
      "qGrid": [0.15, 0.3, 0.45],
      "pOfQ": [68442.7, 68444.1, 68445.8],
      "sOfQ": [10266.41, 20533.23, 30799.61],
      "lOfV": [0.0021, 0.0032, 0.0044],
      "updatedAt": "2026-04-06T09:15:04.000Z"
    }
  ],
  "generatedAt": "2026-04-06T09:15:04.000Z",
  "source": "venues-api"
}
```

### POST `/api/venues/:id/reconnect`
Operator action: принудительный reconnect площадки.

### POST `/api/venues`
Operator action: создать новую запись `venue_config`.

Request body (пример):
```json
{
  "venue_id": "binance",
  "adapter_mode": "simulated",
  "venue_symbol": "BTCUSDT",
  "curve_level": "L2",
  "synthetic_enabled": true
}
```

### POST `/api/venues/:id/disable`
Operator action: выключить площадку для routing.
Проксируется в `venues` Admin API: `POST /api/v1/venues/{id}/disable` (persist + hot reload).

### POST `/api/venues/:id/enable`
Operator action: повторно включить площадку для routing.
Проксируется в `venues` Admin API: `POST /api/v1/venues/{id}/enable` (persist + hot reload).

### POST `/api/venues/:id/routing-mode`
Operator action: задать ручной routing override.
Проксируется в `venues` Admin API: `POST /api/v1/venues/{id}/routing-mode`.

Request body:
```json
{
  "mode": "auto"
}
```

Допустимые значения `mode`:
- `auto`
- `watch`

### PUT `/api/venues/:id/config`
Operator action: обновление `venue_config` (hot reload через `venues` Admin API).

Request body (пример):
```json
{
  "venue_symbol": "BTCUSDT",
  "curve_level": "L2",
  "synthetic_enabled": true,
  "depth_levels": 20,
  "stale_threshold_ms": 3000,
  "circuit_breaker_enabled": true,
  "circuit_breaker_errors": 10,
  "circuit_breaker_window_ms": 30000,
  "circuit_breaker_cooldown_ms": 30000,
  "is_active": true,
  "routing_mode": "auto"
}
```

### DELETE `/api/venues/:id/config`
Operator action: удалить (деактивировать) запись `venue_config`.

## 8. F-12 Hedge PnL Dashboard

### GET `/api/hedge-pnl`
Используется экраном `Hedge PnL Dashboard` для аналитики hedge PnL по времени, символам и venues, включая контроль `referenceMid / clearingPrice` против `avgFillPrice`.

Query params (опционально):
- `symbol`
- `venue`
- `providerId`

Response `200`:
```json
{
  "summary": {
    "totalPnl": -59.01,
    "totalFees": 31.8,
    "netAfterFees": -59.01,
    "grossBeforeFees": -27.21,
    "totalNotional": 50415.03,
    "totalFilledQty": 0.74,
    "reportCount": 2,
    "flowCount": 1,
    "avgSlippageBps": -4.52,
    "avgPriceSpread": -30.8,
    "winRatePct": 0
  },
  "timeSeries": [
    {
      "executionId": "exec-btc-001-01",
      "hedgeFlowId": "hgf-2026-05-04-btc-001",
      "batchId": "batch-2026-05-04-0915",
      "providerId": "provider-alpha",
      "symbol": "BTC/USDT",
      "venueId": "binance",
      "side": "SELL",
      "status": "FILLED",
      "urgency": "HIGH",
      "timestamp": "2026-05-04T06:15:17.000Z",
      "filledQty": 0.72,
      "notional": 49054.68,
      "fee": 29.44,
      "feeCurrency": "USDT",
      "clearingPrice": 68105.25,
      "referenceMid": 68105.25,
      "avgFillPrice": 68131.5,
      "priceSpread": 26.25,
      "slippageBps": 3.85,
      "hedgePnl": -48.34,
      "cumulativePnl": -48.34
    }
  ],
  "symbolBreakdown": [
    {
      "id": "BTC/USDT",
      "hedgePnl": -59.01,
      "fees": 31.8,
      "notional": 50415.03,
      "filledQty": 0.74,
      "reports": 2,
      "avgSlippageBps": -4.52,
      "avgPriceSpread": -30.8,
      "winRatePct": 0
    }
  ],
  "venueBreakdown": [
    {
      "id": "binance",
      "hedgePnl": -48.34,
      "fees": 29.44,
      "notional": 49054.68,
      "filledQty": 0.72,
      "reports": 1
    }
  ],
  "filters": {
    "symbols": ["BTC/USDT"],
    "venues": ["binance"],
    "providers": ["provider-alpha"]
  },
  "generatedAt": "2026-05-04T06:16:00.000Z",
  "source": "mock"
}
```

Фронтенд нормализует контрактные алиасы (`time_series`, `hedge_pnl`, `avg_fill_price`, `reference_mid`, `venue_id`, `symbol_breakdown` и т.п.) к camelCase UI-полям.

## 9. F-12 Execution Live Feed

### GET `/api/executions/live`
Используется экраном `Execution Live Feed` для первичного snapshot потока `execution.venue`.

Query params (опционально):
- `symbol`
- `venue`
- `providerId`
- `status`
- `side`
- `search`
- `limit` (по умолчанию UI запрашивает `50`)

Response `200`:
```json
{
  "items": [
    {
      "executionId": "exec-btc-001-01",
      "hedgeFlowId": "hgf-2026-05-04-btc-001",
      "childOrderId": "chd-btc-001-a",
      "clientOrderId": "hgf-btc-001-01",
      "batchId": "batch-2026-05-04-0915",
      "providerId": "provider-alpha",
      "symbol": "BTC/USDT",
      "venueSymbol": "BTCUSDT",
      "venueId": "binance",
      "side": "SELL",
      "status": "FILLED",
      "urgency": "HIGH",
      "routeType": "IOC",
      "timestamp": "2026-05-04T06:15:17.000Z",
      "receivedAt": "2026-05-04T06:15:17.036Z",
      "filledQty": 0.72,
      "avgFillPrice": 68131.5,
      "referenceMid": 68105.25,
      "notional": 49054.68,
      "fee": 29.44,
      "feeCurrency": "USDT",
      "slippageBps": 3.85,
      "hedgePnl": -48.34,
      "latencyMs": 36,
      "sourceTopic": "execution.venue"
    }
  ],
  "total": 1,
  "summary": {
    "reports": 1,
    "filled": 1,
    "partial": 0,
    "rejected": 0,
    "totalNotional": 49054.68,
    "totalFees": 29.44,
    "avgSlippageBps": 3.85,
    "p95LatencyMs": 36,
    "slippageAlerts": 0
  },
  "filters": {
    "symbols": ["BTC/USDT"],
    "venues": ["binance"],
    "providers": ["provider-alpha"],
    "statuses": ["FILLED"],
    "sides": ["SELL"]
  },
  "generatedAt": "2026-05-04T06:15:18.000Z",
  "source": "mock:execution.venue"
}
```

### WebSocket `/api/executions/live/ws`
После handshake сервер сначала отправляет snapshot, затем live-сообщения `ExecutionReport`.

Сообщения:
```json
{ "type": "snapshot", "payload": { "items": [] } }
```

```json
{
  "type": "execution_report",
  "item": {
    "executionId": "exec-btc-001-01-live-7",
    "sourceExecutionId": "exec-btc-001-01",
    "sequence": 7,
    "live": true
  },
  "generatedAt": "2026-05-04T06:15:20.000Z",
  "source": "mock:execution.venue"
}
```

```json
{ "type": "heartbeat", "generatedAt": "2026-05-04T06:15:22.000Z" }
```

Фронтенд нормализует контрактные алиасы (`execution_id`, `avgPrice`, `avg_price`, `filled_qty`, `venue_id` и т.п.) к UI-полям `executionId`, `avgFillPrice`, `filledQty`, `venueId`.

## 10. F-12 Reconciliation Alerts

### GET `/api/reconciliation-alerts`
Используется экраном `Reconciliation Alerts` для операторского списка событий из `risk.alerts`: `HEDGE_UNDERFILL` и `HEDGE_RISK_REJECT` с привязкой к HedgeFlow, gap и timestamp.

Query params (опционально):
- `severity` (`critical` | `warning` | `info`)
- `status`
- `type`
- `symbol`
- `venue`
- `providerId`
- `search`
- `limit` (по умолчанию UI запрашивает `80`)

Response `200`:
```json
{
  "items": [
    {
      "alertId": "HEDGE_UNDERFILL:hgf-2026-05-04-eth-003",
      "type": "HEDGE_UNDERFILL",
      "severity": "warning",
      "hedgeFlowId": "hgf-2026-05-04-eth-003",
      "intentId": "int-2026-05-04-eth-003",
      "batchId": "batch-2026-05-04-0845",
      "providerId": "provider-alpha",
      "symbol": "ETH/USDT",
      "side": "SELL",
      "status": "UNDERFILLED",
      "reconciliationStatus": "UNDERFILLED",
      "venueIds": ["coinbase", "uniswap_v3"],
      "targetQty": 18,
      "filledQty": 14.2,
      "gapQty": 3.8,
      "gapPct": 21.11,
      "targetNotional": 56880,
      "avgFillPrice": 3154.4,
      "referenceMid": 3160.1,
      "hedgePnl": 38.24,
      "slippageBps": -18.04,
      "totalFee": 42.7,
      "feeCurrency": "USDT",
      "urgency": "MEDIUM",
      "strategy": "VWAP guarded",
      "hedgeMode": "auto_after_batch",
      "timeoutMs": 120000,
      "timestamp": "2026-05-04T05:47:30.000Z",
      "nextAction": "Raise HEDGE_UNDERFILLED and wait for operator override.",
      "statusReason": "CEX_A depth vanished; DEX fallback capped by slippage guard.",
      "riskDecision": "ALLOW",
      "riskLimitUsagePct": 79,
      "sourceTopic": "risk.alerts"
    }
  ],
  "total": 1,
  "summary": {
    "total": 1,
    "critical": 0,
    "warning": 1,
    "underfilled": 1,
    "rejected": 0,
    "totalGapQty": 3.8,
    "avgGapPct": 21.11,
    "oldestAlertAt": "2026-05-04T05:47:30.000Z",
    "latestAlertAt": "2026-05-04T05:47:30.000Z"
  },
  "filters": {
    "symbols": ["ETH/USDT", "SOL/USDT"],
    "venues": ["binance", "coinbase", "uniswap_v3"],
    "providers": ["provider-alpha", "provider-gamma"],
    "statuses": ["RISK_REJECTED", "UNDERFILLED"],
    "severities": ["critical", "warning"],
    "types": ["HEDGE_RISK_REJECT", "HEDGE_UNDERFILL"]
  },
  "generatedAt": "2026-05-04T05:48:00.000Z",
  "source": "mock:risk.alerts"
}
```

Фронтенд нормализует контрактные алиасы (`alert_id`, `hedge_flow_id`, `gap_qty`, `venue_ids`, `risk_limit_usage_pct` и т.п.) к camelCase UI-полям.

## 11. F-12 Manual Override

### GET `/api/manual-overrides`
Используется экраном `Manual Override` для загрузки опций формы, source alerts из `risk.alerts` и последних созданных manual ExecutionIntent.

Response `200`:
```json
{
  "defaults": {
    "side": "SELL",
    "urgency": "MEDIUM",
    "timeoutMs": 120000,
    "providerId": "operator-manual"
  },
  "options": {
    "symbols": ["BTC/USDT", "ETH/USDT"],
    "venues": ["binance", "coinbase", "uniswap_v3"],
    "providers": ["provider-alpha"],
    "sides": ["BUY", "SELL"],
    "urgencies": ["LOW", "MEDIUM", "HIGH"]
  },
  "sourceAlerts": [
    {
      "alertId": "HEDGE_UNDERFILL:hgf-2026-05-04-eth-003",
      "hedgeFlowId": "hgf-2026-05-04-eth-003",
      "symbol": "ETH/USDT",
      "side": "SELL",
      "gapQty": 3.8,
      "venueIds": ["coinbase", "uniswap_v3"]
    }
  ],
  "recent": [],
  "summary": {
    "sourceAlerts": 2,
    "recent": 0,
    "accepted": 0,
    "rejected": 0
  },
  "generatedAt": "2026-05-04T05:48:00.000Z",
  "source": "mock:execution.intents"
}
```

### POST `/api/manual-overrides`
Создание ручного operator ExecutionIntent. Mock API валидирует поля, считает target notional и возвращает статус mock risk check (`ACCEPTED` или `RISK_REJECTED`).

Request body:
```json
{
  "sourceAlertId": "HEDGE_UNDERFILL:hgf-2026-05-04-eth-003",
  "symbol": "ETH/USDT",
  "side": "SELL",
  "targetQty": 3.8,
  "venueIds": ["coinbase"],
  "urgency": "MEDIUM",
  "priceConstraint": 3160.1,
  "timeoutMs": 120000,
  "providerId": "provider-alpha",
  "reason": "Close residual UNDERFILLED exposure."
}
```

Response `201`:
```json
{
  "ok": true,
  "intent": {
    "intentId": "manual-intent-2026-05-04-001",
    "hedgeFlowId": "manual-hgf-2026-05-04-001",
    "sourceAlertId": "HEDGE_UNDERFILL:hgf-2026-05-04-eth-003",
    "sourceHedgeFlowId": "hgf-2026-05-04-eth-003",
    "batchId": "manual-2026-05-04",
    "providerId": "provider-alpha",
    "symbol": "ETH/USDT",
    "side": "SELL",
    "targetQty": 3.8,
    "targetNotional": 12008.38,
    "venueIds": ["coinbase"],
    "urgency": "MEDIUM",
    "priceConstraint": 3160.1,
    "timeoutMs": 120000,
    "reason": "Close residual UNDERFILLED exposure.",
    "referenceMid": 3160.1,
    "maxSlippageBps": 15,
    "riskStatus": "ACCEPTED",
    "riskDecision": "ALLOW",
    "riskReason": "Manual override accepted by mock risk policy.",
    "hedgeMode": "operator_override",
    "routePlan": [
      {
        "venueId": "coinbase",
        "orderType": "IOC",
        "splitQty": 3.8,
        "sequence": 1
      }
    ],
    "sourceTopic": "execution.intents",
    "status": "ACCEPTED"
  },
  "context": {
    "recent": []
  }
}
```

Validation error `400`:
```json
{
  "code": "validation_error",
  "message": "Manual override validation failed.",
  "errors": [
    { "field": "targetQty", "message": "targetQty must be positive" }
  ]
}
```

Фронтенд нормализует контрактные алиасы (`intent_id`, `source_alert_id`, `target_qty`, `venue_ids`, `route_plan` и т.п.) к camelCase UI-полям.

## 12. F-12 Policy Config

### GET `/api/policy-config`
Используется экраном `Policy Config` для чтения runtime `solverconfig`: `hedgeTriggerThreshold`, `hedgeUrgencyPolicy`, `maxSlippageBps` и общих risk limits. Response также содержит impact preview по текущим hedge flows и audit trail.

Response `200`:
```json
{
  "config": {
    "solverConfigId": "solver-prod-v4",
    "revision": 1,
    "hedgeTriggerThreshold": {
      "BTC/USDT": 0.25,
      "ETH/USDT": 5
    },
    "hedgeUrgencyPolicy": {
      "LOW": { "minGapPct": 0, "orderType": "LIMIT", "timeoutMs": 180000 },
      "MEDIUM": { "minGapPct": 10, "orderType": "IOC", "timeoutMs": 120000 },
      "HIGH": { "minGapPct": 25, "orderType": "MARKET", "timeoutMs": 60000 }
    },
    "maxSlippageBps": {
      "LOW": 8,
      "MEDIUM": 15,
      "HIGH": 25
    },
    "riskLimits": {
      "hedgeExposureLimit": 250000,
      "maxNotionalPerHedge": 100000
    },
    "updatedBy": "system",
    "updatedAt": "2026-05-04T06:00:00.000Z"
  },
  "options": {
    "urgencies": ["LOW", "MEDIUM", "HIGH"],
    "orderTypes": ["POST_ONLY", "LIMIT", "IOC", "MARKET"],
    "symbols": ["BTC/USDT", "ETH/USDT"]
  },
  "impact": [
    {
      "hedgeFlowId": "hgf-2026-05-04-btc-001",
      "symbol": "BTC/USDT",
      "status": "UNDERFILLED",
      "gapQty": 0.28,
      "thresholdQty": 0.25,
      "gapPct": 28,
      "eligible": true,
      "urgency": "HIGH",
      "maxSlippageBps": 25
    }
  ],
  "audit": [
    {
      "revision": 1,
      "actor": "system",
      "reason": "Initial F-12 mock solverconfig.",
      "updatedAt": "2026-05-04T06:00:00.000Z",
      "changedFields": ["initial"]
    }
  ],
  "summary": {
    "revision": 1,
    "symbols": 2,
    "eligibleFlows": 1,
    "avgMaxSlippageBps": 16,
    "maxThresholdQty": 5,
    "hedgeExposureLimit": 250000,
    "maxNotionalPerHedge": 100000
  },
  "generatedAt": "2026-05-04T06:01:00.000Z",
  "source": "mock:solverconfig"
}
```

### PUT `/api/policy-config`
Обновление policy config из Admin UI. Mock API валидирует допустимые slippage caps, order type, timeout, thresholds, risk limits и обязательный audit reason.

Request body:
```json
{
  "updatedBy": "operator-ui",
  "reason": "Tighten HIGH urgency slippage after reconciliation alert.",
  "config": {
    "solverConfigId": "solver-prod-v4",
    "hedgeTriggerThreshold": {
      "BTC/USDT": 0.25,
      "ETH/USDT": 5
    },
    "hedgeUrgencyPolicy": {
      "LOW": { "minGapPct": 0, "orderType": "LIMIT", "timeoutMs": 180000 },
      "MEDIUM": { "minGapPct": 10, "orderType": "IOC", "timeoutMs": 120000 },
      "HIGH": { "minGapPct": 25, "orderType": "MARKET", "timeoutMs": 60000 }
    },
    "maxSlippageBps": {
      "LOW": 8,
      "MEDIUM": 15,
      "HIGH": 25
    },
    "riskLimits": {
      "hedgeExposureLimit": 250000,
      "maxNotionalPerHedge": 100000
    }
  }
}
```

Response `200` повторяет контракт `GET /api/policy-config` с увеличенным `revision` и новой записью в `audit`.

Validation error `400`:
```json
{
  "code": "validation_error",
  "message": "Policy config validation failed.",
  "errors": [
    { "field": "hedgeUrgencyPolicy.HIGH.timeoutMs", "message": "timeoutMs must be at least 100" }
  ]
}
```

Фронтенд нормализует контрактные алиасы (`solver_config_id`, `hedge_trigger_threshold`, `hedge_urgency_policy`, `max_slippage_bps`, `risk_limits` и т.п.) к camelCase UI-полям.

## 13. Ошибки, которые ожидает фронтенд

Фронтенд обрабатывает в UI в первую очередь следующие коды:
- `400` (невалидные данные формы/заявки)
- `401` (ошибка логина/токена)
- `403` (недостаточно средств)
- `404` (не найдена заявка/ресурс)
- `409` (email уже зарегистрирован)
- `5xx` (ошибка сервера)
