# F-13 — Post-Trade Report

> **Статус:** Not implemented.

Детальный отчёт по завершённым FlowOrder: VWAP, IS, декомпозиция, экспорт CSV/PDF. Требует ClickHouse и сервис-агрегатор. См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна предоставлять клиенту подробные отчёты по завершённым заявкам: фактический VWAP, IS (среднее, std, ключевые квантили), профиль исполнения во времени, риск-показатели.

- Доступны для скачивания (JSON / CSV / PDF).
- Декомпозиция IS: spread + temporary impact + permanent impact + volatility.
- Регуляторные выгрузки агрегируются по периоду.

Источник: IN-001 §6 FR-REPORT-001/002.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
