---
id: DOC-API-EPOCHS
phase: 06-api
status: implemented (2026-11-02, CR-F18-018 этап Э1)
level: sea
owner: core-team
source:
  - docs/03-architecture/adr/ADR-090-ce-positions-single-owner-uniform-reset.md
related:
  - docs/06-api/rest/reset-positions.md (операция, создающая метку)
  - docs/07-data/ce-reset-epoch.md (таблица меток)
  - docs/changes/CR-F18-018-positions-single-owner-uniform-reset.md
---

# REST: метка сброса позиций (эпоха) — `GET /api/vector-clearing/epochs` и общие правила окон истории

Сброс позиций создаёт **метку (эпоху)**: номер, момент, вид и название эксперимента. Каждый, кто читает историю, обязан знать, к какой эпохе относится его окно, и не
смешивать интервалы до и после сброса (ADR-090 п. 6). Этот документ описывает маршрут чтения меток и **общие правила окон**, которым подчиняются все маршруты истории.

## Эпохи

- Эпоха **0** — виртуальная: «до первой метки», в таблице не хранится.
- Эпоха **N** занимает полуоткрытый интервал `[at_N, at_{N+1})`; у текущей правой границы нет.
- Вид метки: `legacy` (унаследована из прежней метки конфигурации, `trade_stats_reset_at`), `bridge` (сброс кнопкой через прежний вызов ledger: остатки площадок и план по
  активу **не** сброшены), `absolute` (абсолютный сброс контура CE, `tools/reset/absolute_reset.sh`; с этапа Э2 — операция ledger).

## Параметры

| Параметр | Обязателен | Смысл |
| --- | --- | --- |
| `limit` | нет | сколько эпох вернуть (новые первыми); по умолчанию 50, не больше 200 |

## Ответ 200

```json
{
  "supported": true,
  "current": { "epoch": 4, "at_ms": 1793605342000, "kind": "bridge", "note": "опыт D" },
  "epochs": [ { "epoch": 4, "at_ms": 1793605342000, "kind": "bridge", "note": "опыт D" } ],
  "count": 4,
  "has_more": true,
  "warmup_min": 60,
  "ledger_restarts": [1793590000000],
  "ledger_restarts_known_since_ms": 1793000000000,
  "generatedAt": "2026-11-02T10:00:00.000Z"
}
```

- `supported: false` — метки недоступны (Postgres не ответил или таблицы ещё нет): потребитель обязан это показать, а не считать «меток не было»; в этом случае `current: null`.
- Меток ещё не было: `supported: true`, `current` — виртуальная эпоха `{ "epoch": 0, "at_ms": 0, "kind": "virtual", "note": "" }`, `epochs: []`.
- Автор (`actor`) и `boot_id` ledger в публичный ответ **не входят** (автор берётся из заголовка `X-User-Id`, который клиент выставляет сам; снимки состояния до и после сброса тоже
  наружу не отдаются): они хранятся в таблице для аудита.
- `warmup_min` и `current.warmup_until_ms` — прогрев текущей эпохи (OQ-F18v2-62): в первый час модель порогов хеджа копит оценки потока, этот час помечается и по умолчанию исключается из разборов скриптами.
  Длина — `CE_EPOCH_WARMUP_MIN` (по умолчанию 60).
- `ledger_restarts` — моменты перезапусков ledger (последние 20): остатки площадок в памяти ledger при них обнулялись, позиции в Postgres остались ([ce-ledger-boot.md](../../07-data/ce-ledger-boot.md)).
  Не считаются: первое наблюдение BFF и запуск в первые 10 минут после метки `absolute` (его поднимает сам скрипт сброса, состояние там исходное). `ledger_restarts_known_since_ms` — момент, с которого запуски известны
  (`null` — ни одного не наблюдалось).
- Длина прогрева — `CE_EPOCH_WARMUP_MIN` (целое 0…1440, по умолчанию 60); неверное значение игнорируется с записью в журнал BFF (прогрев не выключается молча).
- Ответ кэшируется на 3 секунды.

## Общие правила окон истории (`resolveEpochWindow`)

Одни и те же параметры и блок ответа действуют в маршрутах `pair-trades-history`, `agent-pnl-history`, `pnl-history`, `hedge-model-history`:

| Параметр | Смысл |
| --- | --- |
| `hours` | окно в часах (как прежде; нечисло и 0 — окно по умолчанию, потолок свой у каждого маршрута) |
| `from_ms`, `to_ms` | явное окно в миллисекундах (положительные числа; длина не больше потолка маршрута, иначе 400 `window_too_large`) |
| `epoch` | вся эпоха N целиком (не окно в часах); нельзя вместе с `hours`, `from_ms`, `to_ms` (400 `ambiguous_window`); неизвестный номер — 404 `epoch_not_found`; не целое — 400 `bad_epoch`; очень старая эпоха ограничена потолком окна |
| `allow_cross_reset` | `true` разрешает окну пересекать метки сброса; без него окно **усекается до начала эпохи, в которой лежит его конец** |

Блок в каждом ответе истории:

```json
{
  "epoch_supported": true,
  "epoch": { "epoch": 4, "at_ms": 1793605342000, "kind": "bridge", "note": "опыт D" },
  "window": {
    "requested_from_ms": 1793519000000, "effective_from_ms": 1793605342000, "to_ms": 1793605400000,
    "window_epochs": [ { "epoch": 4, "at_ms": 1793605342000, "kind": "bridge", "note": "опыт D" } ],
    "crosses_reset": true, "truncated_by_reset": true, "allow_cross_reset": false,
    "epoch_requested": null, "clamped_by_max_window": false, "explicit_window": false, "hours": 24
  },
  "reset_markers": [ 1793605342000 ]
}
```

- `crosses_reset` — запрошенное окно содержало метку; `truncated_by_reset` — окно усечено (данные до метки не показаны). `reset_markers` — моменты меток в окне (для вертикальных маркеров графика).
- `window.ledger_restarts` — перезапуски ledger внутри окна, `window.ledger_restarts_known_since_ms` — момент, с которого запуски известны (окно, начавшееся раньше, содержит неизвестные перезапуски). Это граница для
  анализа, а не эпоха: окно не усекается. Метка запуска — момент первого наблюдения BFF, он позже настоящего старта до 90 секунд, поэтому окно, кончающееся в этот промежуток, тоже получает предупреждение.
- При `allow_cross_reset=true` каждая точка (где маршрут возвращает точки) несёт номер своей эпохи `epoch`; накопления (выгода `s`) начинаются заново на метке. Маршрут `pair-trades-history` окно через метку **не
  поддерживает** (накопленные объёмы склеили бы эксперименты): 400 `cross_reset_not_supported`, нужна отдельная выборка по эпохам (`epoch=N`).
- Метки недоступны: `epoch_supported: false`, `epoch: null`, `window_epochs: []`, усечения нет (прежнее поведение без выдумывания границы).
- Заголовок «текущей эпохи» (`epoch_supported`, `epoch`) есть также в ответах `agent-positions`, `agent-pnl`, `system-pnl`.

Выгрузка CSV `hedge-model-history?format=csv`: последняя колонка `epoch_id`, заголовки `X-Epoch`, `X-Window-From-Ms`, `X-Clipped-To-Epoch`, `X-Crosses-Reset`.

## Ошибки

| Код | Когда |
| --- | --- |
| 400 `ambiguous_window` | `epoch` вместе с `hours`, `from_ms` или `to_ms` |
| 400 `bad_epoch`, `bad_window`, `window_too_large` | некорректные параметры окна |
| 404 `epoch_not_found` | нет такой эпохи |
| 409 `epochs_unavailable` | `epoch=N` при недоступных метках |
| 503 `postgres_unavailable` | нет Postgres |
