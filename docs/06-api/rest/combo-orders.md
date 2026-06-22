# REST Endpoint: /v1/combo-orders (F-09)

## Status

TODO / planned — proposed schema sketch. Материализуется code-implementer #11
как HTTP-обёртка над gRPC методами OrderFlowService.

## Transport

REST HTTP/JSON (API Gateway → gRPC OrderFlowService)

---

## POST /v1/combo-orders

Создать `ComboOrder`. Транслируется в `CreateComboOrder` gRPC.

### Request body (JSON)

```json
{
  "client_combo_id": "string (idempotency key)",
  "account_id": "string",
  "combo_type": "pair|basket|spread|conditional|oco|bracket",
  "execution_mode": "orchestration_only|multileg_vector_solver",
  "atomicity_policy": "strict_atomic|scalable_atomic|best_effort|sequential_fallback|external_compensating",
  "atomicity_scope": "internal_batch|venue_native|external_compensating|none",
  "fallback_policy": "scale_down|skip_batch|best_effort|cancel_group",
  "ratio_basis": "quantity|notional_weight",
  "min_execution_scale": "number (0.0–1.0)",
  "max_ratio_deviation_bps": "integer",
  "legs": [
    {
      "leg_id": "string",
      "instrument": "BTC/USDT",
      "side": "buy|sell",
      "ratio": "number (if ratio_basis=quantity)",
      "weight": "number (if ratio_basis=notional_weight)",
      "price_low": "string (decimal)",
      "price_high": "string (decimal)",
      "max_rate": "string (decimal, base/sec)",
      "max_qty": "string (decimal)",
      "venue_preferences": ["internal"]
    }
  ],
  "constraints": [
    {
      "constraint_id": "string",
      "constraint_type": "spread_range|max_weight_deviation|max_total_notional|...",
      "coefficients": {"BTCUSDT": 1.0, "ETHUSDT": -15.0},
      "lower_bound": "string (decimal)",
      "upper_bound": "string (decimal)",
      "severity": "hard|soft"
    }
  ]
}
```

Денежные поля (`price_low`, `price_high`, `max_rate`, `max_qty`, `lower_bound`,
`upper_bound`) передаются как строки (decimal) → маппятся в `fob.common.v1.Decimal`.
Никогда не `double`/`float`.

### Response (201 Created)

```json
{
  "combo_id": "string",
  "status": "active|waiting_for_trigger|rejected|throttled",
  "leg_order_ids": {"leg_btc": "order_id_...", "leg_eth": "order_id_..."},
  "error": null
}
```

---

## POST /v1/combo-orders/preview

Grouped preview без создания заявки. Транслируется в `PreviewComboOrder` gRPC.

### Request body

Аналогичен `POST /v1/combo-orders` без `client_combo_id`.

### Response (200 OK)

```json
{
  "expected_execution_scale": "string (decimal)",
  "expected_leg_fills": [
    {
      "leg_id": "string",
      "expected_qty": "string (decimal)",
      "expected_price": "string (decimal)",
      "expected_notional": "string (decimal)",
      "is_binding": true
    }
  ],
  "expected_combined_vwap": "string (decimal)",
  "expected_is": "string (decimal)",
  "expected_ratio_deviation_bps": 0,
  "expected_margin_load": "string (decimal)",
  "binding_leg_ids": ["leg_eth"],
  "binding_constraint_ids": ["ctr_total_budget"],
  "has_external_execution_risk": false
}
```

---

## GET /v1/combo-orders/{combo_id}

Получить статус `ComboOrder`. Транслируется в `GetComboOrder` gRPC.

### Response (200 OK)

```json
{
  "combo_id": "string",
  "status": "active|partially_filled|filled|cancelled|degraded|...",
  "legs": [...],
  "last_execution_group_id": "string",
  "cumulative_scale": "string (decimal)",
  "ratio_deviation_bps": 0
}
```

---

## DELETE /v1/combo-orders/{combo_id}

Отменить `ComboOrder`. Транслируется в `CancelComboOrder` gRPC.

### Response (200 OK)

```json
{
  "success": true,
  "status": "cancelled"
}
```

---

## POST /v1/batch-orders

Создать `BatchOrder`. Транслируется в `CreateBatchOrder` gRPC.

### Request body

```json
{
  "client_batch_id": "string (idempotency key)",
  "account_id": "string",
  "execution_mode": "orchestration_only|multileg_vector_solver",
  "start_at": "ISO8601",
  "end_at": "ISO8601",
  "child_refs": [
    {"child_type": "combo", "child_ref": "co_01"}
  ]
}
```

### Response (201 Created)

```json
{
  "batch_order_id": "string",
  "status": "active|waiting_for_trigger|rejected"
}
```

---

## Auth

Все endpoint-ы требуют аутентификации. `user_id` извлекается из JWT / session;
не принимается из тела запроса.

## Rate limits

Применяются на уровне API Gateway. Throttled responses: `HTTP 429`,
`status: throttled` в теле ответа.

## Related Contract

- [gRPC: order-flow-create-combo-order.md](../grpc/order-flow-create-combo-order.md)

## Used In Features

- [F-09](../../02-system/features/F-09-batch-combo-orders/)

## Source Fragments

- IN-011 §12.2 (API Gateway responsibilities), §11.1 (create flow)
```

---
