---
id: ADR-058
title: CE — committed/in-flight состояние агентов вместо cooldown-таймера хеджа
status: accepted
date: 2026-09-10
accepted: 2026-09-10
level: sea
feature: F-18
related: [ADR-054, ADR-055, ADR-057]
supersedes_rule: заменяет CE_HEDGE_COOLDOWN_MS (таймер) на состояние committed в PG
sources: [CE_algorithm_spec.md, CE_virtual_counterparties.md]  # IN-0XX (ingest-docs)
---

# ADR-058 — committed/in-flight вместо cooldown

> **Решение владельца (2026-09-10, `CE_algorithm_spec.md` §A6,A8,Р-10; `CE_virtual_counterparties.md` §6).**
> Между решением клиринга и подтверждением исполнения проходит время. Защита от повторной
> отправки — **состояние `committed` по агенту** (отправлено, не подтверждено), а не
> таймер `CE_HEDGE_COOLDOWN_MS`. Клиринг считает свободное = `qty − committed`.

## Контекст

Сейчас роль «не отправить дважды» играет таймер
[`CE_HEDGE_COOLDOWN_MS`](../../../cpp/risk/src/app/risk_uc.cpp) (Р-10): если площадка
отвечает дольше кулдауна, тот же хедж уйдёт снова; если быстрее — эмиссия задержится
искусственно. Задача не решается временем — она решается учётом уже обещанного объёма.
Пересчёт №1 (§A6) фиксирует `committed`, пересчёт №2 (§A8) — факт по `execution.reports`.

## Решение

- Таблица PG `agent_state(agent_id, committed, assigned_last, updated_at)`.
- `matching` при эмиссии: `committed_e += |f_e|` в одной транзакции с публикацией
  поручений (idempotent по `batch_id`).
- Клиринг следующего такта видит `свободное = qty − committed` и корректирует якорь:
  `σ* −= ρ·committed` (§A2).
- Снятие: `execution.reports`/timeout/cancel уменьшают `committed`; отменённые/просроченные
  возвращают остаток и шлют `risk.alerts`.
- `CE_HEDGE_COOLDOWN_MS` удаляется.

**Реализация:** `matching` (запись `committed` в PG в транзакции с эмиссией), `ledger`/`risk`
(снятие по `execution.reports`), удаление cooldown-ветки в `risk_uc`.

## Альтернативы

- **Cooldown-таймер (текущий)** — не покрывает медленные/быстрые площадки; гонки. Отвергнут.
- **Идемпотентность только по `intent_id` без `committed`** — не даёт клирингу знать
  занятый объём, повторно назначает ту же ногу. Недостаточно.

## Последствия

- Новая PG-таблица + транзакционная граница «эмиссия ⇄ committed».
- Тест-регресс двойной отправки (§7): два такта подряд без подтверждений — суммарный
  назначенный объём по агенту ≤ `capacity`.
- Уровень агента позиции (ADR-057) получает реальный источник.

## Обратимость

Флагом `CE_COMMITTED_STATE_ENABLED`: при выключении — прежний cooldown. Таблица аддитивна;
при откате не читается.
