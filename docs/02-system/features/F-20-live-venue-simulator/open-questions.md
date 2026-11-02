# Открытые вопросы — F-20 Live Venue Simulator

> Формат: `OQ-F20-NN` · **статус** (`open` / `blocking` / `resolved`) ·
> **адресат** (кто решает — конкретный агент/роль, не просто «кто-то»).
> `blocking` = нельзя продолжать implementation этой фичи, пока не снято.

### OQ-F20-01 — у F-20 нет FR-* бизнес-требования уровня functional-requirements.md

- **Вопрос.** При нормализации `feature.yaml.parentRequirements` (Этап 1
  п.6, ADR-086/IN-019, 2026-10-27) обнаружено: у 17 из 19 фич (F-01..F-17)
  и у F-05A каждый `FR-*` в `docs/02-system/functional-requirements.md`
  явно называет свою фичу (`Acceptance: F-XX`) — перенос в
  `feature.yaml.parentRequirements` был чисто механическим. У F-20 такого
  `FR-*` нет вообще: Live Venue Simulator (SIM_ONLY/LIVE_ONLY/SHADOW)
  появился позже исходного документа бизнес-требований, из отдельного
  запроса на безопасное тестирование хеджа без реального выхода на рынок.
  Нужно решение: писать retroactive `FR-SIM-*` сейчас (кто формулирует
  требование постфактум — задача не инженерная), или сознательно оставить
  F-20 без этого уровня трассировки (UC-F20-01/02 и ADR уже покрывают её
  достаточно)? Тот же вопрос у F-18 (см. его `open-questions.md`,
  `OQ-F18v2-25`).
- **Влияет на:** `feature.yaml.parentRequirements` (сейчас `[]`),
  полноту traceability-цепочки Business Requirement → Feature.
- **Статус:** `open`.
- **Адресат:** владелец (решение продуктовое — писать ли FR-* постфактум).
- **Источник:** Этап 1 п.6 (ADR-086/IN-019); см. также
  `docs/00-methodology/known-gaps.md`.
