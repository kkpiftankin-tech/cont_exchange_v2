---
spec_id: UI-F16-COMP-001
feature: F-09 (MVP-6 slice 4)
feature_yaml: docs/02-system/features/F-09-batch-combo-orders/feature.yaml
ops_page: true
route: /combo-compensation-live
chrome: ops
status: draft
date: 2026-06-14
authors:
  - frontend-architect
adrs:
  - docs/03-architecture/adr/ADR-039-compensation-resolution.md
  - docs/03-architecture/adr/ADR-040-compensation-resolution-cross-service.md
grpc_contracts:
  - docs/06-api/grpc/matching-compensation-service.md
  - docs/06-api/grpc/order-flow-resolve-compensation.md
rest_api: docs/06-api/rest/combo-compensations.md
---

# Экран: Combo Compensation Resolution — live

**URL:** `/combo-compensation-live`
**Chrome:** ops (`.profile-container` + `.hedge-shell` + `navbar-main hedge-navbar`, unified PR-F02-013)
**Feature / MVP:** F-09 MVP-6, ADR-039, ADR-040
**Цель:** Operator-driven разрешение `pending`-компенсаций combo. Когда внешняя нога combo упала (rejected / timeout / cancelled) а внутренние ноги уже исполнены, запись появляется в этой таблице. Оператор явно выбирает действие (reverse / retry / accept). Авто-резолв запрещён (ADR-039 §1).

## Место в ops-навигации

Добавить `<a href="/combo-compensation-live">` (i18n `navbar.comboComp` = «comp») в `navbar-main hedge-navbar` на всех ops-страницах, после `/policy-config-live`, перед `/sim-sessions`.

## REST API (node BFF `frontend/api/server.js`)

Контракт: `docs/06-api/rest/combo-compensations.md`.

### GET `/api/v1/combo-compensations?status=pending`
gRPC backing: `matching.CompensationService/ListPendingCompensations`.
```json
{ "compensations": [ {"compensationId","parentOrderId","legId","reason","internalFilledQty"} ], "generatedAt": "ISO8601" }
```
`internalFilledQty` — строка (Decimal). Рендерить as-is, без float (CLAUDE.md §9). Информационный снимок; реальный объём реверса считается из `combo_order_legs.filled_cum` (ADR-039 §3).

### POST `/api/v1/combo-compensations/{compensationId}/resolve`
gRPC backing: `order_flow.OrderFlowService/ResolveCompensation`.
Request: `{ "action": "reverse_internal|retry_external|accept", "operatorId": "non-empty" }`.
Response: `{ "applied": bool, "reversingOrderIds": [..], "error": {code,message}|null }`.
- `applied:true` — выполнено.
- `applied:false, error:null` — уже resolved (идемпотентный no-op, не ошибка; ADR-040 §5).
- `applied:false, error:{...}` — ошибка (неизвестный action / NOT_IMPLEMENTED для retry_external / внутренняя).

## Состояния страницы
1. **Auth check** — `isAuthenticated()` → redirect `/login`; до проверки `loading-screen`.
2. **Loading** (первый load / ручное «Обновить») — spinner.
3. **Empty (healthy default)** — нет pending: «Нет pending-компенсаций» + live-индикатор.
4. **Error** — текст ошибки + кнопка «Повторить».
5. **Success** — таблица pending-записей.
6. **In-flight resolve** — кнопки строки disabled + «…», другие строки активны.
7. **Resolved (applied:true)** — строка зелёная, показывает `reversingOrderIds` (моноширинно, усечение UUID); исчезает на следующем poll.
8. **Idempotent no-op (applied:false, error:null)** — жёлтый «Уже разрешено (no-op)».
9. **Action error** — красный текст ошибки, кнопки остаются для retry.

## Layout
- **Hero:** kicker `F-09 MVP-6 / ADR-039`; h1; подзаголовок (источник, polling 5s); справа «Обновить» + `● live 5s` + timestamp.
- **Stats cards (4):** Pending / Resolved (сессия) / Реверс / Принято — client-side из ответа + журнал сессии.
- **Toolbar:** фильтр по `reason` (client-side): Все / rejected / timeout / cancelled.
- **Таблица (колонки):** Comp ID, Parent Order, Leg ID (UUID усечён 18 + title=full), Reason (badge: rejected красный / timeout жёлтый / cancelled серый), Internal Filled Qty (Decimal as-is + tooltip «снимок»), Действие.

## Action selector (per row)
Radio-group:
- `reverse_internal` — Реверс внутренней экспозиции (REAL MONEY).
- `accept` — Принять экспозицию (dismiss). **default** (наименее деструктивно).
- `retry_external` — **disabled** (tooltip: «Недоступно в MVP-6, реализация MVP-7, NOT_IMPLEMENTED»).

Кнопка «Выполнить»:
- `accept` → inline confirm (без модала).
- `reverse_internal` → **модальное окно подтверждения** (обязательно — money action).

## Confirmation modal (reverse_internal)
- Заголовок: «Подтверждение реверса (REAL MONEY ACTION)».
- Warning-блок (выделенный фон): создаёт реверсивные FlowOrder(s); объём из `combo_order_legs.filled_cum`, не из снимка; необратимо.
- Info: compensation_id / parent_order_id / leg_id / reason / qty snapshot (info only).
- Поле **Operator ID** (обязательное, ≥3 симв.; hint «Записывается в audit log, ADR-039 §4 / §22»).
- Кнопки: «Подтвердить реверс» (disabled пока operatorId невалиден) / «Отмена».
- POST `resolve` с `{action:"reverse_internal", operatorId}`; во время — disabled «…»; после — закрыть, обновить строку.

## Polling / refresh
- `useInterval` 5000ms (паттерн ManualOverrideLive / ReconciliationAlertsLive).
- Silent poll (без loading state); после resolve — немедленный `load()`.
- Resolved строки исчезают на следующем poll (backend отдаёт только `status=pending`).
- Кнопка «Обновить» = `load({showLoader:true})`.

## Компоненты
- **ComboCompensationLive** (роут): state `isAuth/data/loading/error/reasonFilter/resolvingId/rowStates/sessionResolvedCount`.
- **CompensationTable** (items, rowStates, onResolve, resolvingId).
- **CompensationRow** (item, rowState, onResolve, isResolving): selectedAction, showInlineConfirm, showModal.
- **ReverseConfirmModal** (item, isOpen, isSubmitting, onConfirm(operatorId), onClose): operatorId + validation.
- **ReasonBadge** (reason) — технические значения без i18n.
- **CompStatsCards** (pending, sessionResolved, sessionReverse, sessionAccept).

## Acceptance criteria
| # | Условие | Ожидание |
|---|---|---|
| AC-UI-COMP-001 | пустой список | empty state, нет таблицы/ошибок |
| AC-UI-COMP-002 | 3 записи разных reason | 3 строки, badges корректно окрашены |
| AC-UI-COMP-003 | `internalFilledQty="0.00000001"` | рендер строки as-is, без `Number()`/округления |
| AC-UI-COMP-004 | accept → «Принять» | inline confirm → POST `{action:"accept",operatorId}` |
| AC-UI-COMP-005 | reverse_internal → «Реверс...» | модал; «Подтвердить» disabled пока operatorId пуст |
| AC-UI-COMP-006 | operatorId="" в модале | submit заблокирован, нет POST |
| AC-UI-COMP-007 | `{applied:true,reversingOrderIds:["abc"]}` | «Resolved. Reversing orders: abc…» |
| AC-UI-COMP-008 | `{applied:false,error:null}` | «Уже разрешено (no-op)», жёлтый, без ошибки |
| AC-UI-COMP-009 | `{applied:false,error:{...}}` | сообщение об ошибке, кнопки активны |
| AC-UI-COMP-010 | retry_external option | disabled + tooltip, не открывает модал |
| AC-UI-COMP-011 | POST in-flight для строки #1 | строка #1 disabled, строка #2 активна |
| AC-UI-COMP-012 | poll после resolve | resolved строка исчезает |
| AC-UI-COMP-013 | GET 500 | error state + «Повторить» |
| AC-UI-COMP-014 | фильтр reason=rejected | только rejected строки |
| AC-UI-COMP-015 | hard refresh | грузит актуальный `main.<hash>.js` (указывать при деплое) |

## i18n
Все строки под ключом `comboComp.*` в `frontend/web/public/locales/{ru,en}/translation.json` (+ `navbar.comboComp`). Полный список ключей — см. реализацию; критичные: `comboComp.empty.title/body`, `comboComp.modal.title/warning/operatorId/confirm`, `comboComp.action.{resolved,alreadyResolved,retryNotImpl,reversingOrders}`, `comboComp.errors.{loadFailed,resolveFailed}`.

## CSS
- `.comp-page` + `.CompensationResolutionLive.css`; shell `.profile-container`+`.hedge-shell`; `.comp-hero`, `.comp-cards/.comp-card`, `.comp-table-wrap/.comp-table`.
- Badges `.comp-reason-badge .reason-{rejected|timeout|cancelled}`.
- Row states `.comp-row-{resolved|noop|error}`; modal `.comp-modal-overlay/.comp-modal`, warning `.comp-modal-warning` (red-tinted bg).

## Трассировка
| Тип | Путь |
|---|---|
| Feature | `docs/02-system/features/F-09-batch-combo-orders/feature.yaml` |
| ADR | ADR-039, ADR-040 |
| gRPC | `docs/06-api/grpc/matching-compensation-service.md`, `docs/06-api/grpc/order-flow-resolve-compensation.md` |
| REST | `docs/06-api/rest/combo-compensations.md` |
| Data | `docs/07-data/oltp-schema.md` (`combo_compensations`) |

## TODO до/в реализации
1. `docs/06-api/rest/combo-compensations.md` — REST-контракт (готовится вместе с этим spec).
2. `frontend/api/server.js` — 2 handler'а (GET list → matching gRPC, POST resolve → order_flow gRPC).
3. i18n `comboComp.*` в ru/en.
4. Navbar `navbar.comboComp` на всех ops-страницах.
5. Router `<Route path="/combo-compensation-live">` в App.js.
6. `frontend/web/src/pages/ComboCompensation/ComboCompensationLive.{js,css}`.
7. Frontend Scope в `docs/02-system/features/F-09-batch-combo-orders/README.md`.
