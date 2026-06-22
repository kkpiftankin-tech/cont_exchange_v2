<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F15-03. A/B сравнение двух replay-сессий

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-15-backtest-replay](../../features/F-15-backtest-replay/) |
| ☁️ L0 system sequence | [SEQ-UC-F15-03-system](sequences/SEQ-UC-F15-03-system.md) — system как чёрный ящик |
| 🌊 L1 service sequence | [SEQ-F15-03-cancel-services](../../../05-components/sequences/SEQ-F15-03-cancel-services.md) — взаимодействие сервисов |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-15. Backtest / Replay](../../features/F-15-backtest-replay/)

## Primary Actor

Analyst или Administrator.

## Preconditions

- Сессии `sessionA` и `sessionB` существуют, статус `completed` (или обе `partial` с одинаковым date range).
- У сессий совместимые `date_range_from`/`date_range_to`.
- Пользователь имеет permission `replay:read`.

## Trigger

REST `GET /api/v1/replay/compare?sessionA={id}&sessionB={id}`.

## Main Flow

1. RBAC `replay:read` + ownership (admin видит все; analyst/viewer — только свои).
2. `CompareReplaySessionsUC.ValidatePair`:
   - Оба ID существуют.
   - Оба статуса в `{completed, cancelled+partial}` с непустым `replay_summaries`.
   - Date ranges совпадают (или допускается пересечение по конфигу).
3. Проверка кэша `replay_compare_cache` по ключу `min(A,B):max(A,B)`. Если свежий — возврат.
4. Иначе чтение обоих `replay_summaries`. Расчёт `delta = B - A` для метрик: `avg_is`, `total_pnl`, `sharpe`, `fill_rate`, `max_drawdown`, `avg_solve_time_ms`.
5. Для каждой метрики вычисляется `better ∈ {A, B, equal}` с учётом direction-aware: `avg_is` — меньше лучше; `total_pnl`, `sharpe`, `fill_rate` — больше лучше; `max_drawdown`, `avg_solve_time_ms` — меньше лучше.
6. Кэширование результата в `replay_compare_cache.comparison_json`.
7. Возврат `200 OK` со структурой `CompareResponse`.

## Alternative Flows

### A1. Сессии не найдены

`404 Not Found` если хотя бы один ID отсутствует.

### A2. Несовместимые статусы

`400 Bad Request` с `error_code=incompatible_status`, например один `completed`, второй `pending`.

### A3. Несовместимые date range

`400 Bad Request` с `error_code=date_range_mismatch`.

## Postconditions

- Запись в `replay_compare_cache` (TTL по конфигу).
- `audit_log` — read access.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F15-03-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F15-02-replay-cycle-services.md) (commonly reuses replay cycle sequence)

## Related Contracts

- [REST `GET /api/v1/replay/compare`](../../../06-api/rest/replay.md#get-apiv1replaycompare)

## Related Components

- [backtest-service](../../../05-components/backtest-service/overview.md)

## Related Data

- [`replay_summaries`](../../../07-data/replay-summaries.md), [`replay_compare_cache`](../../../07-data/replay-sessions.md#таблица-replay_compare_cache)

## Source Fragments

- IN-006-FR-014 (F15-14), IN-006-FR-046..047 (validation, delta)
