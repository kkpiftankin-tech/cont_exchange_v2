# F-03 — FlowOrder Lifecycle (Amend / Cancel)

> **Статус:** частично реализовано (только Cancel, с критическим багом).

## Описание

Управление активной заявкой после её создания: изменение параметров (amend), отмена, автоматическое истечение по `time_in_force`. Каждое изменение проходит повторный risk-check.

## Связанные

- F-02 (Create FlowOrder) — родитель сущности
- F-04 (Batch Clearing) — потребитель cancel/amend событий
- F-07 (Pre-trade Risk) — повторный гейт

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна позволять клиенту изменять параметры активной заявки (`p_low`, `p_high`, `q_rate`, `q_max`, окно) и полностью её отменять.

- Изменения проходят повторный pre-trade risk check.
- При amend система пересчитывает preview VWAP/IS.
- При cancel неисполненная часть освобождает резерв в Ledger.
- История изменений сохраняется для audit.

Источник: IN-001 §6 FR-ORDER-003, §5.1.2.

## Source Fragments

- IN-001-FR-027
- IN-001-FR-028
