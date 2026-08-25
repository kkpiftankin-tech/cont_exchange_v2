# gRPC Contract: OrderFlowService — Combo/Batch Methods (F-09)

## Status

materialized (T-F09-010/012) — `contracts/proto/fob/orders/v1/combo.proto` создан, `order_flow_service.proto` расширен 5 rpc; protoc-валидация зелёная, backward-compatible.

## Purpose

Расширение `fob.orders.v1.OrderFlowService` методами для работы с пакетными и многоногими
заявками (F-09). Покрывает полный жизненный цикл: создание, предварительный расчёт, запрос
состояния и отмену.

Архитектурное основание: ADR-031 (режимы исполнения и атомарность), ADR-032 (parent/child
модель), ADR-033 (топик execution.groups).

## Transport

gRPC (unary, за исключением PreviewComboOrder — может быть server-streaming в будущем).

## Service

`fob.orders.v1.OrderFlowService` (extension materialized, backward-compatible — только новые rpc; существующие CreateFlowOrder/CancelFlowOrder/GetFlowOrder/ListFlowOrders неизменны).

---

## Methods

### CreateBatchOrder

```proto
rpc CreateBatchOrder(CreateBatchOrderRequest) returns (CreateBatchOrderResponse);
```

Создать `BatchOrder` — клиентский parent object, объединяющий несколько дочерних
`ComboOrder`/`FlowOrder` или условных ветвей.

**Caller:** gateway (REST `POST /v1/batch-orders` → gRPC)  
**Callee:** order-flow

**Idempotency:** по `client_batch_id`. Повторный вызов с тем же `client_batch_id`
возвращает уже созданный `batch_order_id` без дублирования.

---

### CreateComboOrder

```proto
rpc CreateComboOrder(CreateComboOrderRequest) returns (CreateComboOrderResponse);
```

Создать `ComboOrder` — многоногую заявку с общими constraints, execution mode и atomicity
policy. Может быть standalone или вложена в `BatchOrder`.

**Caller:** gateway (REST `POST /v1/combo-orders` → gRPC)  
**Callee:** order-flow

**Idempotency:** по `client_combo_id`. Все ноги нормализуются и сохраняются атомарно
(все или ни одной). Повторный вызов с тем же `client_combo_id` возвращает существующий
`combo_id`.

---

### PreviewComboOrder

```proto
rpc PreviewComboOrder(PreviewComboOrderRequest) returns (PreviewComboOrderResponse);
```

Предварительный расчёт grouped preview без создания заявки: ожидаемый execution scale,
leg fills, combined VWAP, IS, ratio deviation, binding leg, active constraints,
external execution risk, expected margin load.

Preview считает grouped prognosis (не независимые прогнозы по ногам).

**Caller:** gateway (REST `POST /v1/combo-orders/preview` → gRPC), Web UI  
**Callee:** order-flow (проксирует в Execution Planning)

**Idempotency:** read-only, idempotent by design.

---

### CancelComboOrder

```proto
rpc CancelComboOrder(CancelComboOrderRequest) returns (CancelComboOrderResponse);
```

Отменить `ComboOrder` и все её активные ноги. OCO-сиблинги отменяются идемпотентно.
Активные `FlowOrder` дочерних ног переходят в статус `cancelled`.

**Caller:** gateway (REST `DELETE /v1/combo-orders/{combo_id}` → gRPC)  
**Callee:** order-flow

**Idempotency:** по `combo_id` + `reason`. Повторная отмена уже отменённой/заполненной
заявки возвращает success без side-effects.

---

### GetComboOrder

```proto
rpc GetComboOrder(GetComboOrderRequest) returns (GetComboOrderResponse);
```

Получить текущее состояние `ComboOrder`: parent status, leg statuses, filled quantities,
ratio deviation, last execution group reference.

**Caller:** gateway (REST `GET /v1/combo-orders/{combo_id}` → gRPC), Web UI  
**Callee:** order-flow

**Idempotency:** read-only, idempotent by design.

---

## Proto Schema (sketch — contracts/proto/fob/orders/v1/combo.proto)

> Ниже — авторизованный черновик. Файл `contracts/proto/fob/orders/v1/combo.proto`
> создаёт code-implementer #11. Теги полей зафиксированы как часть контракта
> (изменение тегов — breaking change, требует ADR).

### Enums

```proto
enum ExecutionMode {
  EXECUTION_MODE_UNSPECIFIED      = 0;
  EXECUTION_MODE_ORCHESTRATION_ONLY     = 1;
  EXECUTION_MODE_MULTILEG_VECTOR_SOLVER = 2;
}

enum AtomicityPolicy {
  ATOMICITY_POLICY_UNSPECIFIED         = 0;
  ATOMICITY_POLICY_STRICT_ATOMIC       = 1;
  ATOMICITY_POLICY_SCALABLE_ATOMIC     = 2;
  ATOMICITY_POLICY_BEST_EFFORT         = 3;
  ATOMICITY_POLICY_SEQUENTIAL_FALLBACK = 4;
  ATOMICITY_POLICY_EXTERNAL_COMPENSATING = 5;
}

enum AtomicityScope {
  ATOMICITY_SCOPE_UNSPECIFIED           = 0;
  ATOMICITY_SCOPE_INTERNAL_BATCH        = 1;
  ATOMICITY_SCOPE_VENUE_NATIVE          = 2;
  ATOMICITY_SCOPE_EXTERNAL_COMPENSATING = 3;
  ATOMICITY_SCOPE_NONE                  = 4;
}

enum ComboType {
  COMBO_TYPE_UNSPECIFIED  = 0;
  COMBO_TYPE_PAIR         = 1;
  COMBO_TYPE_BASKET       = 2;
  COMBO_TYPE_SPREAD       = 3;
  COMBO_TYPE_CONDITIONAL  = 4;
  COMBO_TYPE_OCO          = 5;
  COMBO_TYPE_BRACKET      = 6;
}

enum RatioBasis {
  RATIO_BASIS_UNSPECIFIED     = 0;
  RATIO_BASIS_QUANTITY        = 1;  // ratio задаётся в единицах базового актива
  RATIO_BASIS_NOTIONAL_WEIGHT = 2;  // веса считаются по номинальной стоимости
}

enum ConstraintSeverity {
  CONSTRAINT_SEVERITY_UNSPECIFIED = 0;
  CONSTRAINT_SEVERITY_HARD        = 1;
  CONSTRAINT_SEVERITY_SOFT        = 2;
}

enum ConditionalLinkType {
  CONDITIONAL_LINK_TYPE_UNSPECIFIED    = 0;
  CONDITIONAL_LINK_TYPE_OCO_SIBLING    = 1;
  CONDITIONAL_LINK_TYPE_BRACKET_ENTRY  = 2;  // entry → TP/SL
  CONDITIONAL_LINK_TYPE_CONDITIONAL    = 3;
}

enum LegStatus {
  LEG_STATUS_UNSPECIFIED        = 0;
  LEG_STATUS_INACTIVE           = 1;
  LEG_STATUS_ACTIVE             = 2;
  LEG_STATUS_WAITING_FOR_TRIGGER = 3;
  LEG_STATUS_PARTIALLY_FILLED   = 4;
  LEG_STATUS_FILLED             = 5;
  LEG_STATUS_CANCELLED          = 6;
  LEG_STATUS_BLOCKED_BY_GROUP   = 7;
  LEG_STATUS_BLOCKED_BY_ATOMICITY = 8;
  LEG_STATUS_FAILED_EXTERNAL    = 9;
  LEG_STATUS_COMPENSATED        = 10;
}

enum ParentOrderStatus {
  PARENT_ORDER_STATUS_UNSPECIFIED      = 0;
  PARENT_ORDER_STATUS_DRAFT            = 1;
  PARENT_ORDER_STATUS_RISK_PENDING     = 2;
  PARENT_ORDER_STATUS_ACTIVE           = 3;
  PARENT_ORDER_STATUS_WAITING_FOR_TRIGGER = 4;
  PARENT_ORDER_STATUS_PARTIALLY_FILLED = 5;
  PARENT_ORDER_STATUS_FILLED           = 6;
  PARENT_ORDER_STATUS_CANCELLED        = 7;
  PARENT_ORDER_STATUS_EXPIRED          = 8;
  PARENT_ORDER_STATUS_DEGRADED         = 9;
  PARENT_ORDER_STATUS_ROLLBACK_PENDING = 10;
  PARENT_ORDER_STATUS_ROLLEDBACK       = 11;
  PARENT_ORDER_STATUS_REJECTED         = 12;
  PARENT_ORDER_STATUS_THROTTLED        = 13;
}
```

### Core messages

```proto
// Одна нога комбинированной заявки.
message Leg {
  string leg_id                          = 1;
  string parent_order_id                 = 2;
  fob.common.v1.Instrument instrument    = 3;
  fob.common.v1.Side side               = 4;

  // Соотношение (если ratio_basis = QUANTITY).
  // Знак кодируется через side; значение >= 0.
  fob.common.v1.Decimal ratio           = 5;

  // Целевой вес (если ratio_basis = NOTIONAL_WEIGHT). Диапазон (0, 1].
  fob.common.v1.Decimal weight          = 6;

  // Ценовой диапазон исполнения (quote per base).
  fob.common.v1.Decimal price_low       = 7;
  fob.common.v1.Decimal price_high      = 8;

  // Максимальная скорость (base units/sec) и максимальный объём.
  fob.common.v1.Decimal max_rate        = 9;
  fob.common.v1.Decimal max_qty         = 10;

  // Кумулятивно исполненный объём ноги.
  fob.common.v1.Decimal filled_cum      = 11;

  // Предпочтения по источникам ликвидности (например, ["internal", "binance"]).
  repeated string venue_preferences     = 12;

  LegStatus status                      = 13;
}

// Общее ограничение группы ног.
message MultiLegConstraint {
  string constraint_id                  = 1;
  string parent_order_id               = 2;

  // Тип: "ratio_eq", "max_weight_deviation", "spread_range",
  //       "max_total_notional", "factor_neutrality", "margin_cap".
  string constraint_type               = 3;

  // Коэффициенты линейной комбинации по символу.
  // Для spread: {BTCUSDT: 1.0, ETHUSDT: -15.0}.
  map<string, double> coefficients     = 4;

  // Нижняя и верхняя допустимые границы значения комбинации.
  // Для max_weight_deviation: lower = -valueBps, upper = valueBps.
  fob.common.v1.Decimal lower_bound    = 5;
  fob.common.v1.Decimal upper_bound    = 6;

  ConstraintSeverity severity          = 7;
}

// Ребро графа активации / взаимоотмены.
message ConditionalLink {
  string link_id                        = 1;
  string parent_order_id               = 2;
  ConditionalLinkType link_type        = 3;

  // Источник и цель в графе (leg_id или child combo_id).
  string source_ref                    = 4;
  string target_ref                    = 5;

  // Свободный текст для условия (например, "filled > 0").
  string trigger_condition             = 6;
}

// Клиентский BatchOrder (родительский объект-контейнер).
message BatchOrder {
  string batch_order_id                 = 1;
  string user_id                        = 2;
  string account_id                     = 3;

  // Тип: "batch", "combo", "basket", "spread", "conditional", "oco", "bracket".
  string order_type                     = 4;

  ExecutionMode execution_mode          = 5;
  ParentOrderStatus status              = 6;

  google.protobuf.Timestamp start_at    = 7;
  google.protobuf.Timestamp end_at      = 8;

  // Ссылки на дочерние элементы (combo_id или order_id).
  repeated ChildRef child_refs          = 9;

  google.protobuf.Timestamp created_at  = 10;
  google.protobuf.Timestamp updated_at  = 11;
}

message ChildRef {
  // "combo" | "flow_order" | "conditional_branch"
  string child_type = 1;
  string child_ref  = 2;
}

// ComboOrder — многоногая заявка.
message ComboOrder {
  string combo_order_id                      = 1;
  // Опционально: если входит в BatchOrder.
  string batch_order_id                      = 2;
  string user_id                             = 3;
  string account_id                          = 4;

  ComboType combo_type                       = 5;
  ExecutionMode execution_mode               = 6;
  AtomicityPolicy atomicity_policy           = 7;
  AtomicityScope atomicity_scope             = 8;

  // Политика резервного поведения при деградации.
  // "scale_down" | "skip_batch" | "best_effort" | "cancel_group".
  string fallback_policy                     = 9;

  RatioBasis ratio_basis                     = 10;

  // Минимально допустимый масштаб исполнения (0.0–1.0).
  // Для strict_atomic: не применяется (всё или ничего).
  // Для scalable_atomic: группа не исполняется если alpha < min_execution_scale.
  fob.common.v1.Decimal min_execution_scale  = 11;

  // Максимально допустимое отклонение соотношения, в базисных пунктах.
  uint32 max_ratio_deviation_bps             = 12;

  repeated Leg legs                          = 13;
  repeated MultiLegConstraint constraints    = 14;
  repeated ConditionalLink graph_links       = 15;

  ParentOrderStatus status                   = 16;

  google.protobuf.Timestamp created_at       = 17;
  google.protobuf.Timestamp updated_at       = 18;
}
```

### Request / Response messages

```proto
// --- CreateBatchOrder ---

message CreateBatchOrderRequest {
  fob.common.v1.EventMeta meta = 1;
  // Idempotency key от клиента.
  string client_batch_id       = 2;
  string user_id               = 3;
  string account_id            = 4;
  ExecutionMode execution_mode = 5;
  repeated ChildRef child_refs = 6;
  google.protobuf.Timestamp start_at = 7;
  google.protobuf.Timestamp end_at   = 8;
}

message CreateBatchOrderResponse {
  fob.common.v1.EventMeta meta   = 1;
  bool accepted                  = 2;
  string batch_order_id          = 3;
  ParentOrderStatus status        = 4;
  fob.common.v1.Error error      = 5;
}

// --- CreateComboOrder ---

message CreateComboOrderRequest {
  fob.common.v1.EventMeta meta        = 1;
  // Idempotency key от клиента.
  string client_combo_id              = 2;
  string user_id                      = 3;
  string account_id                   = 4;
  // Опционально: привязать к существующему BatchOrder.
  string batch_order_id               = 5;
  ComboType combo_type                = 6;
  ExecutionMode execution_mode        = 7;
  AtomicityPolicy atomicity_policy    = 8;
  AtomicityScope atomicity_scope      = 9;
  string fallback_policy              = 10;
  RatioBasis ratio_basis              = 11;
  fob.common.v1.Decimal min_execution_scale = 12;
  uint32 max_ratio_deviation_bps      = 13;
  repeated Leg legs                   = 14;
  repeated MultiLegConstraint constraints = 15;
  repeated ConditionalLink graph_links = 16;
}

message CreateComboOrderResponse {
  fob.common.v1.EventMeta meta   = 1;
  bool accepted                  = 2;
  string combo_id                = 3;
  // leg_id → child FlowOrder id (для режима orchestration_only).
  map<string, string> leg_order_ids = 4;
  ParentOrderStatus status        = 5;
  fob.common.v1.Error error      = 6;
}

// --- PreviewComboOrder ---

message PreviewComboOrderRequest {
  fob.common.v1.EventMeta meta        = 1;
  string user_id                      = 3;
  string account_id                   = 4;
  ComboType combo_type                = 6;
  ExecutionMode execution_mode        = 7;
  AtomicityPolicy atomicity_policy    = 8;
  AtomicityScope atomicity_scope      = 9;
  RatioBasis ratio_basis              = 10;
  repeated Leg legs                   = 11;
  repeated MultiLegConstraint constraints = 12;
}

message LegPreview {
  string leg_id                         = 1;
  fob.common.v1.Decimal expected_qty    = 2;
  fob.common.v1.Decimal expected_price  = 3;
  fob.common.v1.Decimal expected_notional = 4;
  bool is_binding                        = 5;
}

message PreviewComboOrderResponse {
  fob.common.v1.EventMeta meta                  = 1;
  // Ожидаемый масштаб исполнения (0.0–1.0).
  fob.common.v1.Decimal expected_execution_scale = 2;
  repeated LegPreview expected_leg_fills         = 3;
  // combined VWAP по всей группе в quote currency.
  fob.common.v1.Decimal expected_combined_vwap   = 4;
  // Ожидаемое отклонение от эталонной цены (IS), в quote currency.
  fob.common.v1.Decimal expected_is              = 5;
  // Ожидаемое отклонение соотношения, в базисных пунктах.
  uint32 expected_ratio_deviation_bps            = 6;
  // Ожидаемая маржинальная нагрузка в quote currency.
  fob.common.v1.Decimal expected_margin_load     = 7;
  // Ноги, ограничивающие общий масштаб.
  repeated string binding_leg_ids                = 8;
  // Активные ограничения.
  repeated string binding_constraint_ids         = 9;
  // Признак: хотя бы одна нога требует внешнего исполнения.
  bool has_external_execution_risk               = 10;
  fob.common.v1.Error error                      = 11;
}

// --- CancelComboOrder ---

message CancelComboOrderRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id               = 2;
  string combo_id              = 3;
  string reason                = 4;
}

message CancelComboOrderResponse {
  fob.common.v1.EventMeta meta  = 1;
  bool success                  = 2;
  // Статус после отмены (cancelled / filled / already_terminal).
  ParentOrderStatus status       = 3;
  fob.common.v1.Error error     = 4;
}

// --- GetComboOrder ---

message GetComboOrderRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id               = 2;
  string combo_id              = 3;
}

message ComboOrderView {
  ComboOrder order                         = 1;
  // Ссылка на последний ExecutionGroup этой заявки.
  string last_execution_group_id           = 2;
  // Суммарно исполненный масштаб с начала жизни заявки.
  fob.common.v1.Decimal cumulative_scale   = 3;
  uint32 ratio_deviation_bps               = 4;
  fob.common.v1.Error error                = 5;
}

message GetComboOrderResponse {
  fob.common.v1.EventMeta meta = 1;
  ComboOrderView view          = 2;
}
```

### Service diff (order_flow_service.proto)

Добавить в `service OrderFlowService` — только новые rpc, существующие методы не трогать:

```proto
// F-09: Batch/Combo/Multi-leg orders (ADR-031, ADR-032, ADR-033)
rpc CreateBatchOrder(CreateBatchOrderRequest) returns (CreateBatchOrderResponse);
rpc CreateComboOrder(CreateComboOrderRequest) returns (CreateComboOrderResponse);
rpc PreviewComboOrder(PreviewComboOrderRequest) returns (PreviewComboOrderResponse);
rpc CancelComboOrder(CancelComboOrderRequest) returns (CancelComboOrderResponse);
rpc GetComboOrder(GetComboOrderRequest) returns (GetComboOrderResponse);
```

Импортировать в `order_flow_service.proto`:

```proto
import "fob/orders/v1/combo.proto";
```

---

## Idempotency Summary

| Method | Idempotency key | Semantics |
|---|---|---|
| `CreateBatchOrder` | `client_batch_id` | повтор → вернуть существующий `batch_order_id` |
| `CreateComboOrder` | `client_combo_id` | повтор → вернуть существующий `combo_id`, ноги не дублировать |
| `PreviewComboOrder` | — | read-only, безопасен |
| `CancelComboOrder` | `combo_id` | идемпотентен: повторная отмена уже отменённой = success |
| `GetComboOrder` | — | read-only, безопасен |

---

## Backward Compatibility

Все новые методы добавляются в сервис без изменения существующих rpc. Новый файл
`combo.proto` не меняет `orders.proto` и `order_flow_service.proto` по смыслу
существующих полей. Теги 1–16 в `FlowOrder` неизменны. Добавление импорта в
`order_flow_service.proto` — backward-compatible.

---

## Used In Features

- [F-09. Batch/Combo/Multi-leg Orders](../../02-system/features/F-09-batch-combo-orders/)

## Used In Use Cases

- [UC-F09-01. Создание многоногой заявки](../../02-system/use-cases/UC-F09-01-create-combo-order/use-case.md)
- [UC-F09-02. Grouped matching в batch cycle](../../02-system/use-cases/UC-F09-02-grouped-matching/use-case.md)
- [UC-F09-03. External leg execution](../../02-system/use-cases/UC-F09-03-external-leg-execution/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F09-UC-F09-01-services](../../05-components/sequences/SEQ-F09-UC-F09-01-services.md)
- [SEQ-F09-UC-F09-02-services](../../05-components/sequences/SEQ-F09-UC-F09-02-services.md)
- [SEQ-F09-UC-F09-03-services](../../05-components/sequences/SEQ-F09-UC-F09-03-services.md)

## Related ADR

- [ADR-031 — Режимы исполнения и атомарность](../../03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md)
- [ADR-032 — Parent/child модель](../../03-architecture/adr/ADR-032-parent-child-order-model.md)
- [ADR-033 — Топик execution.groups](../../03-architecture/adr/ADR-033-execution-groups-topic.md)

## Related Components

- [order-flow](../../05-components/order-flow/overview.md)
- [matching-fob-core](../../05-components/matching-fob-core/overview.md)
- [risk-manager](../../05-components/risk-manager/overview.md)

## Related Data Objects

- [oltp-schema.md — batch_orders, combo_orders, combo_order_legs, combo_constraints, conditional_links](../../07-data/oltp-schema.md)

## Source Fragments

- IN-011 §4 (термины), §7 (UC), §8 (доменная модель), §11.1 (создание), §11.2 (preview), §15 (статусы)
- ADR-031, ADR-032
```

---
