---
id: DOC-API-RESET-POSITIONS
phase: 06-api
status: implemented (2026-11-02, CR-F18-018 этап Э1; мост — абсолютный сброс только скриптом)
level: sea
owner: core-team
source:
  - docs/03-architecture/adr/ADR-090-ce-positions-single-owner-uniform-reset.md
related:
  - docs/06-api/rest/epochs.md (метка и окна истории)
  - docs/07-data/ce-reset-epoch.md
  - docs/11-operations/runbooks/ce-absolute-reset.md (абсолютный сброс и анализ эпохи)
  - docs/changes/CR-F18-018-positions-single-owner-uniform-reset.md
---

# REST: `POST /api/vector-clearing/reset-positions` — сброс позиций агентов с меткой

Операторская деструктивная команда кнопки «Сбросить позиции». Сбрасывает в ledger позиции агентов (`c_j`, `in_flight`), книги заработка R/U и band-хеджи и **создаёт метку (эпоху)**
в `ce_reset_epoch`, по которой все графики и разборы отделяют данные до и после сброса.

> **Этап Э1 («мост»).** Ledger до этапа Э2 не обнуляет остатки площадок, план по активу `ce_committed_`, очередь переводов и пороги агентов, поэтому метка имеет вид `bridge`, а в ответе
> `absolute: false`. Начало эксперимента **с исходного состояния** — `tools/reset/absolute_reset.sh` (метка `absolute`). С этапа Э2 сброс — единая абсолютная операция ledger.

## Запрос

`Content-Type: application/json` (иначе 415, защита от подделки запроса из браузера). Тело:

| Поле | Обязательно | Смысл |
| --- | --- | --- |
| `note` | да | название эксперимента, строка 1–200 знаков без управляющих символов (включая NUL) |
| `confirm` | да | `true` — подтверждение необратимой операции |
| `expected_epoch` | да | номер текущей эпохи, который видит экран (из `GET /epochs`); при расхождении 409 `epoch_changed` — защита от повторного нажатия и устаревшего экрана |
| `request_id` | нет | идентификатор запроса, 8–80 знаков `[A-Za-z0-9_-]`; не задан — BFF создаёт сам. Повтор с тем же `request_id`, актором и названием возвращает прежний результат без нового сброса |
| `agent_ids` | нет | выборочный сброс **не поддерживается**: непустой список — 400 `selective_reset_not_supported` |

Автор операции — заголовок `X-User-Id` (пока не доказательство личности: маршрут без авторизации, известный долг).

## Порядок выполнения (защита от «сброса без следа»)

1. Проверка защиты от подделки запроса, разрешения вне production (`CE_RESET_ALLOWED`), входных полей. Любой отказ пишется в аудит оператора (`ce_reset_positions_denied`).
2. Один сброс за раз (процессный замок) и блокировка Postgres `pg_advisory_xact_lock` в транзакции: проверка `request_id` (повтор), ожидаемой эпохи, интервала между сбросами (не чаще раза в минуту),
   часов (последняя метка не из будущего). Всё, что способно отказать, проверяется **до** ledger.
3. Запись в аудит попытки (`ce_reset_positions_attempt`), снимок состояния до сброса (остатки площадок точными строками, агрегаты агентов), вызов ledger `ResetAgentPositions` (все агенты).
4. Отказ ledger — транзакция откатывается, метка не создаётся (`ce_reset_positions_failed`, этап `ledger`).
5. Метка открывается функцией Postgres `ce_epoch_open(...)` и фиксируется; сбой после сброса в ledger — 502 `epoch_not_recorded` (позиции уже сброшены; повтор с тем же `request_id` допишет метку), запись
   `ce_reset_positions_failed`, этап `epoch_after_ledger_reset`.
6. Запасная метка конфигурации обновляется (откат на прежнюю версию BFF), база прибыли системы удаляется (будет создана заново), в аудит пишется исход (`ce_reset_positions`), кэши очищаются.

## Ответ 200

```json
{
  "epoch": 4, "reset_at_ms": 1793605342000, "kind": "bridge", "note": "опыт D", "request_id": "…",
  "cleared": 68, "scope": "all", "absolute": false, "trade_stats_reset": true, "snapshot_complete": true,
  "warning": "сброшены позиции, книги и хеджи агентов; остатки площадок, план по активу и очередь переводов ledger не сброшены — …"
}
```

Повтор: `replay: true`, `cleared: null`. `snapshot_complete: false` — снимок до сброса неполный (сброс при этом выполнен, факт записан в аудит).

## Ошибки

| Код | Когда |
| --- | --- |
| 400 | `selective_reset_not_supported`, `note_required`, `confirm_required`, `bad_request_id`, `expected_epoch_required`, `bad_body` |
| 403 | `reset_not_allowed` — вне песочницы (`CE_RESET_ALLOWED=0`, либо не задан при `NODE_ENV=production`) |
| 409 | `epoch_changed` (с полем `current`), `request_id_conflict` (тот же `request_id` с другим автором или названием), `reset_in_progress`, `epoch_clock_skew` |
| 415 | не JSON |
| 429 | `reset_too_frequent` — чаще раза в минуту |
| 502 | `grpc_error` (ledger не выполнил сброс, метка не создана), `epoch_not_recorded` (позиции сброшены, метка не записана), `ledger_unavailable`, `reset_failed` |
| 503 | `postgres_unavailable`, `epoch_schema_unavailable` |

## Аудит оператора (`operator_audit_log`)

Действия: `ce_reset_positions_attempt`, `ce_reset_positions` (исход), `ce_reset_positions_denied` (с кодом и статусом), `ce_reset_positions_failed` (с этапом). Источник адреса — заголовки прокси (`X-Real-IP`,
`X-Forwarded-For`), то есть данные клиента (долг: доверенный прокси, см. known-gaps).
