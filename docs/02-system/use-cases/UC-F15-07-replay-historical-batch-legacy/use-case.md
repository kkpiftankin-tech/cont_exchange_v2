---
id: UC-F15-07
title: Replay исторического batch (legacy, historical/superseded)
level: sea
feature: F-15
system-sequence: sequences/SEQ-UC-F15-07-system.md
status: superseded  # draft | active | superseded | deprecated
---

# UC-F15-07 (legacy). (Historical / Superseded) — Replay исторического batch

> **Перенумеровано 2026-10-27** ([ADR-086](../../../03-architecture/adr/ADR-086-two-axis-reply-routing.md)
> Этап 1, IN-019 §8 Этап 1 п.2): этот use case раньше ошибочно делил номер
> `UC-F15-01` с [UC-F15-01. Create Replay Session](../UC-F15-01-create-replay-session/use-case.md).
> Контент не менялся, только номер (01 → 07) и путь (`git mv`).

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-15-backtest-replay](../../features/F-15-backtest-replay/) |
| ☁️ L0 system sequence | [SEQ-UC-F15-07-system](sequences/SEQ-UC-F15-07-system.md) — system как чёрный ящик (historical) |
| 🌊 L1 service sequence | N/A — исторический артефакт; актуальная — у [UC-F15-01 Create Replay Session](../UC-F15-01-create-replay-session/use-case.md) |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

> **Статус:** superseded. Этот UC был placeholder и теперь разбит на 6 более
> конкретных use case'ов после ингеста IN-006.

## Преемники

- [UC-F15-01 Create Replay Session](../UC-F15-01-create-replay-session/use-case.md) — создание сессии и запуск replay-цикла.
- [UC-F15-02 Cancel Replay Session](../UC-F15-02-cancel-replay-session/use-case.md) — отмена выполняющейся / pending сессии.
- [UC-F15-03 A/B Compare Sessions](../UC-F15-03-ab-compare-sessions/use-case.md) — сравнение двух completed сессий.
- [UC-F15-04 Audit Mode Replay](../UC-F15-04-audit-mode-replay/use-case.md) — replay одного `batch_id` + diff vs production (≈ original UC).
- [UC-F15-05 Retry Failed Session](../UC-F15-05-retry-failed-session/use-case.md) — повторный запуск failed / cancelled.
- [UC-F15-06 Determinism Check](../UC-F15-06-replay-determinism-check/use-case.md) — проверка детерминизма повторных прогонов.

## Feature

- [F-15. Backtest / Replay](../../features/F-15-backtest-replay/)

## Note

Старая `sequences/SEQ-UC-F15-07-system.md` (до 2026-10-27 — `SEQ-UC-F15-01-system.md`)
оставлена как historical artifact; актуальные system-sequence — в каждом из
6 новых UC.
