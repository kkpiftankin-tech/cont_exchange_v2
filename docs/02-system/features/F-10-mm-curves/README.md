# F-10 — Market-Maker Liquidity Curves

> **Статус:** Not implemented.

Полноценная поддержка непрерывных CSLO-кривых от маркет-мейкеров с slope-функцией price↔speed. См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна позволять маркет-мейкеру публиковать и изменять непрерывные кривые цен и скоростей с параметрами `P_L`, `P_H`, `Q`, `U`, slope, а также видеть результат своего участия в реальном времени.

- MM может в любой момент изменить параметры кривой; обновление учитывается в следующем batch.
- В UI отображаются PnL, текущий inventory, доля toxic flow.
- Inventory caps работают как hard-stop.

Источник: IN-001 §6 FR-MM-001/002, §5.2 сценарии MM.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
