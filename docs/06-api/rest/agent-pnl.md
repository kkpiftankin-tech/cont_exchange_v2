---
id: DOC-API-AGENT-PNL
phase: 06-api
status: accepted (2026-10-28, ADR-087, модель C′)
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
      "internal": { "has_data": true, "u_has_data": true, "r": 10.0, "u": 0.0, "r_plus_u": 10.0 },
      "external": { "has_data": true, "u_has_data": true, "r": 93.33, "u": 36.67, "r_plus_u": 130.0 },
      "total": { "has_data": true, "u_has_data": true, "r": 103.33, "u": 36.67, "r_plus_u": 140.0 },
      "mark_age_ms": 1200,
      "ext_fills": 3,
      "since_ms": 1792420000000
    }
  ],
  "totals": {
    "internal": { "has_data": true, "u_has_data": true, "r": 10.0, "u": 0.0, "r_plus_u": 10.0 },
    "external": { "has_data": true, "u_has_data": true, "r": 93.33, "u": 36.67, "r_plus_u": 130.0 },
    "total": { "has_data": true, "u_has_data": true, "r": 103.33, "u": 36.67, "r_plus_u": 140.0 },
    "agent_count": 1,
    "incomplete_agents": 0
  },
  "by_kind": {
    "translator": { "agent_kind": "translator", "internal": { "has_data": true, "u_has_data": true, "r": 10.0, "u": 0.0, "r_plus_u": 10.0 }, "external": { "has_data": true, "u_has_data": true, "r": 93.33, "u": 36.67, "r_plus_u": 130.0 }, "total": { "has_data": true, "u_has_data": true, "r": 103.33, "u": 36.67, "r_plus_u": 140.0 }, "agent_count": 1, "incomplete_agents": 0 }
  },
  "generatedAt": "2026-10-28T12:00:00.000Z"
}
```

| Поле | Смысл |
| --- | --- |
| `internal` | внутренняя биржа CE: R — выгода сделок относительно справедливой цены пары в момент сделки (купил ниже — плюс, продал выше — плюс); U всегда 0, потому что на внутренней бирже всё реализуется в такте клиринга. Сумма R по всем агентам равна нулю |
| `external` | внешние биржи (все вместе): R — закрытая хеджем часть позиции (качество исполнения плюс дрейф справедливой цены с момента появления позиции); U — ещё не захеджированный остаток по текущей справедливой цене |
| `total` | внутренняя плюс внешние |
| `has_data` / `u_has_data` | R и U известны независимо. `has_data: false` — R неизвестен (учёт приостановлен или нет курса валюты котировки); `u_has_data: false` — U неизвестен (остаток есть, текущей справедливой цены ещё нет, например сразу после перезапуска ledger). Неизвестное равно `null` (не 0); `r_plus_u` задан только при обоих признаках. Нулевое число при признаке `true` означает «измерено, ноль» |
| `mark_age_ms` | возраст цены оценки, мс; `null` — цена неизвестна. Цена обновляется только дельтами агента, поэтому у агента без потока U «замирает» |
| `ext_fills` | число внешних хедж-исполнений с запуска ledger |
| `since_ms` | начало накопления (миграция учёта или сброс позиций); `null` — не начато |
| `totals` | строка «все агенты»: R — сумма по агентам с известным R, U — по агентам с известным U; `incomplete_agents` — сколько агентов с неизвестной частью; пока такие есть, `r_plus_u` итогов равен `null` |
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

Таблица на вкладке «PnL» над разделами хедж-PnL: две строки шапки (группы «Внутренняя биржа CE», «Внешние биржи», «Итого»; в каждой R, U, R+U), строка на агента, строка «все агенты». Агенты упорядочены по виду (переводчики, затем связки) и идентификатору. Над таблицей — пояснение R и U простыми словами и время начала накопления; неизвестное U показывается прочерком, R при этом виден. Браузер ничего не считает.
