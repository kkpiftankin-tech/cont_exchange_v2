<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F15-04. Audit-mode replay одного batch

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-15-backtest-replay](../../features/F-15-backtest-replay/) |
| ☁️ L0 system sequence | [SEQ-UC-F15-04-system](sequences/SEQ-UC-F15-04-system.md) — system как чёрный ящик |
| 🌊 L1 service sequence | [SEQ-F15-04-audit-mode-services](../../../05-components/sequences/SEQ-F15-04-audit-mode-services.md) — взаимодействие сервисов |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-15. Backtest / Replay](../../features/F-15-backtest-replay/)

## Primary Actor

Auditor / Administrator (`admin`).

## Preconditions

- В ClickHouse есть запись `batchresults` для запрошенного `batch_id` (production output).
- Доступны fills и marketdata_snapshots на момент батча.
- Пользователь имеет permission `replay:execute` + admin role (audit для чужих batch).
- Доступен snapshot решающих конфигов на момент production-батча (`config_version` в `batchresults`).

## Trigger

REST `POST /api/v1/replay/audit-runs` (или специализированный путь, если будет) с payload `{batch_id, tolerance, override_config?}`.

## Main Flow

1. RBAC `replay:execute`.
2. Insert `replay_audit_runs` со `status='pending'`, `batch_id`, `requested_by`.
3. `RunReplayAuditBatchUC`:
   1. Загружает production `BatchResult` из `batchresults` по `batch_id`.
   2. Загружает FlowOrder snapshot и `marketdata_snapshot` на момент батча.
   3. Восстанавливает snapshot конфигов (`solver_config@version`, `risk_limits@version`, `fee_model`).
   4. Вызывает `Solver/Solve` в isolation mode тем же кодом, что обработал production-батч.
   5. Записывает replay BatchResult в `replay_audit_runs.replay_result_json`.
4. Diff:
   - `delta_clear_prices` (per-instrument relative diff)
   - `delta_executed_rates` (per-order)
   - `delta_residual_norm`
   - `delta_fills` (set diff)
5. `IsEquivalent` сравнивает все скаляры с `tolerance_json` (default `{residual_norm: 1e-9, clear_price_rel: 1e-6}`).
6. `UPDATE replay_audit_runs SET equivalent={true|false}, diff_json=..., status='completed', completed_at=now()`.
7. Возврат `200 OK` с `audit_run_id`, `equivalent`, `diff`.

## Alternative Flows

### A1. batch_id не существует

`404 Not Found`.

### A2. Snapshot конфигов на момент батча недоступен

`400 Bad Request` `error_code=config_snapshot_unavailable`.

### A3. Replay solver не сошёлся

`status='completed', equivalent=false`, в `diff.residual_norm` отражено. Без crash.

### A4. Replay solver throw

`status='failed', error_details=...`.

## Postconditions

- `replay_audit_runs` обновлён (один из терминальных статусов).
- `audit_log` — выполненное действие.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F15-04-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F15-04-audit-mode-services.md)

## Related Contracts

- [REST `POST /api/v1/replay/audit-runs`](../../../06-api/rest/replay.md#post-apiv1replayaudit-runs)

## Related Components

- [backtest-service](../../../05-components/backtest-service/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)

## Related Data

- [`replay_audit_runs`](../../../07-data/replay-sessions.md#таблица-replay_audit_runs)
- `batchresults` (ClickHouse, read-only)

## Source Fragments

- IN-006-FR-013 (F15-13), IN-006-FR-044..045 (diff structure, tolerance)
