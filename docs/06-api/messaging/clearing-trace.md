# Kafka Topic: `matching.clearing_trace`

## Status

Planned (топик + CH-таблица + писатель — T-CEA-401, ADR-059)

## Purpose

Единый аудит-артефакт **одного такта CE** (ADR-059): всё, что нужно фронту и разбору
инцидентов, одной записью с общим `batch_id`. Контракт «фронт ничего не считает» (ADR-053):
все точки кривых, координаты, позиции и инварианты приходят с бэкенда.

## Producer

- matching (после `Solve` и проверки инвариантов)

## Consumers

- frontend-api (BFF: панели В-1…В-10)
- observability
- ClickHouse sink → таблица `clearing_trace`

## Key / Retention

- Key: `batch_id`
- Retention: 30 d (аудит; long-retention, отдельно от market-data)

## Payload (`ClearingTrace`)

```text
ClearingTrace {
  batch_id, ts_window, window_ms
  venues[]       { venue, symbol, snapshot_ts, age_ms, included, exclude_reason }
  agents[]       { agent_id, type, leg, node_from, node_to, m_raw, shift_stock,
                   shift_committed, anchor, alpha, dead_zone, capacity,
                   committed_before, flow, x_buy, x_sell }
  nodes[]        { node, price_pm, price_abs, balance_residual,
                   qty_before, delta_immediate, delta_after_transit, target }
  book_levels[]  { venue, side, level, price, size, cum_value, dist_pm }
  curve_points[] { agent_id, sigma, flow }
  marks[]        { asset, mu_pm, mu_abs }
  invariants[]   { code, value, tolerance, passed }     // И-Т1..И-Т6
  pnl            { gross, quad, fees, net, closed_form }
  position       { by_agent[], by_node[], by_asset[] }
  orders[]       { agent_id, venue, side, qty, order_type, price, zone, thresholds }
  timings_ms     { collect, build_agents, assemble, solve, invariants, emit }
}
```

Финальная proto-форма (`fob.matching.v1.ClearingTrace`) — при реализации T-CEA-401; JSON-поля
по образцу `vector_clearing_results`.

## Invariant gate (ADR-059)

Инварианты И-Т1…И-Т6 — стадия конвейера до эмиссии. Нарушение И-Т1 (баланс узлов) или И-Т3
(позиция тремя способами) → такт `FAILED`, заявки не эмитятся, событие в `risk.alerts`,
трасса помечается `failed`.

## Used In

- Feature [F-05A](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)
- Use case [UC-F05A-06](../../02-system/use-cases/UC-F05A-06-ce-agent-tact-clearing/use-case.md)
- Sequence [SEQ-F05A-UC-F05A-06-services](../../05-components/sequences/SEQ-F05A-UC-F05A-06-services.md)

## Related ADR

- [ADR-059 clearing_trace](../../03-architecture/adr/ADR-059-ce-clearing-trace-audit.md)

Принцип «фронт ничего не считает» (весь domain-расчёт на бэкенде) — проектная конвенция;
трасса несёт готовые точки кривых и координаты, фронт только рендерит.
