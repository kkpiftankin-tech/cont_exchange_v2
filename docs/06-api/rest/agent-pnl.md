---
id: DOC-API-AGENT-PNL
phase: 06-api
status: proposed (2026-10-28, ADR-087)
level: sea
owner: core-team
source:
  - docs/02-system/features/F-18-ce-capital-position-hedge/feature.yaml (AC-F18v2-30)
related:
  - docs/06-api/grpc/ledger-agent-pnl.md (контракт ledger, из которого читает маршрут)
  - docs/04-domain/calculations/CALC-CE-AGENT-PNL.md (формулы)
  - docs/03-architecture/adr/ADR-087-ce-agent-pnl-internal-external-total.md (решение)
---

# REST: GET /api/vector-clearing/agent-pnl

Маршрут BFF (`frontend/api/server.js`, чистая функция сборки ответа `frontend/api/agent_pnl_dto.js`). Отдаёт заработок агентов CE для таблицы «Заработок агентов» на вкладке «PnL» (`/hedge-pnl-live`): по каждому агенту R, U и R+U для внутренней биржи, для внешних бирж и итого, в USDT. Все числа посчитаны в ledger (`GetAgentPnl`); BFF только переименовывает поля и не выполняет арифметику над R и U.

## Параметры

| Параметр | Обязателен | Смысл |
| --- | --- | --- |
| `agent_ids` | нет | список агентов через запятую; пусто — все |

## Ответ 200

```json
{
  "currency": "USDT",
  "enabled": true,
  "agents": [
    {
      "agent_id": "T_BTC_binance",
      "agent_kind": "translator",
      "asset": "BTC",
      "quote": "USDT",
      "venues": ["binance"],
      "internal": { "has_data": true, "r": 60.0, "u": 40.0, "r_plus_u": 100.0 },
      "external": { "has_data": true, "r": 40.0, "u": -20.0, "r_plus_u": 20.0 },
      "total": { "has_data": true, "r": 100.0, "u": 20.0, "r_plus_u": 120.0 },
      "mark_age_ms": 1200,
      "ext_fills": 3,
      "since_ms": 1792420000000
    }
  ],
  "totals": {
    "internal": { "has_data": true, "r": 60.0, "u": 40.0, "r_plus_u": 100.0 },
    "external": { "has_data": true, "r": 40.0, "u": -20.0, "r_plus_u": 20.0 },
    "total": { "has_data": true, "r": 100.0, "u": 20.0, "r_plus_u": 120.0 },
    "agent_count": 1,
    "incomplete_agents": 0
  },
  "by_kind": {
    "translator": { "agent_kind": "translator", "internal": { "has_data": true, "r": 60.0, "u": 40.0, "r_plus_u": 100.0 }, "external": { "has_data": true, "r": 40.0, "u": -20.0, "r_plus_u": 20.0 }, "total": { "has_data": true, "r": 100.0, "u": 20.0, "r_plus_u": 120.0 }, "agent_count": 1, "incomplete_agents": 0 }
  },
  "generatedAt": "2026-10-28T12:00:00.000Z"
}
```

| Поле | Смысл |
| --- | --- |
| `internal` | внутренняя биржа CE: R — результат покупок и продаж по средней себестоимости, U — остаток, ещё не проданный другим агентам, по цене оценки |
| `external` | внешние биржи (все вместе): R и U хедж-исполнений |
| `total` | внутренняя плюс внешние |
| `has_data` | `false` — нет цены оценки; тогда `r`, `u`, `r_plus_u` равны `null` (не 0). Нулевое число при `has_data: true` означает «измерено, ноль» |
| `mark_age_ms` | возраст цены оценки, мс; `null` — цена неизвестна. Цена обновляется только дельтами агента, поэтому у агента без потока U «замирает» |
| `ext_fills` | число внешних хедж-исполнений с запуска ledger |
| `since_ms` | начало накопления (миграция учёта или сброс позиций); `null` — не начато |
| `totals` | строка «все агенты»: суммы по агентам, у которых в группе есть данные; `incomplete_agents` — сколько агентов неполные |
| `by_kind` | те же суммы по видам агентов (`translator`, `arbitrageur`), ключ — вид; записи нет, если агентов этого вида нет |
| `enabled` | `false` — ledger запущен с `CE_AGENT_PNL_ENABLED=0`: список пуст |

Числа округляются только при отображении; BFF передаёт значения `Decimal` ledger как JSON number.

## Другие потребители того же расчёта

Панель «Живые позиции» (`GET /api/vector-clearing/agent-positions`) и семплер истории заработка получают R, U, R+U из этого же вызова ledger (`GetAgentPnl`), а не считают сами: колонка «заработок R / U» строки — внешняя группа агента, `pnl_totals` в корне ответа — `by_kind` и `totals`. Поля описаны в [agent-pnl-history](agent-pnl-history.md) §2 (ADR-087 п. 10).

## Ошибки

| Код | Причина |
| --- | --- |
| 502 | ledger недоступен, превышен срок ожидания или метод не реализован (`{"error":"ledger_unavailable","reason":"..."}`). Недоступность не выглядит как «нет данных»: пустой список `agents: []` с кодом 200 бывает только при `enabled: false` или когда агентов нет |

## Экран

Таблица на вкладке «PnL» над разделами хедж-PnL: две строки шапки (группы «Внутренняя биржа CE», «Внешние биржи», «Итого»; в каждой R, U, R+U), строка на агента, строка «все агенты». Агенты упорядочены по виду (переводчики, затем связки) и идентификатору. Над таблицей — пояснение R и U простыми словами и время начала накопления. Браузер ничего не считает.
