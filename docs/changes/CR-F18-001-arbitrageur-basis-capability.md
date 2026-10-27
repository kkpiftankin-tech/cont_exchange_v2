---
id: CR-F18-001
title: Базис и открытая позиция арбитражёра — владеющая capability и исполняемый пример
type: refine
source: review
anchor: UI-VC-POS-BASIS
feature: F-18
capability: CAP-F18-ARBITRAGEUR-BASIS
top: Р2
class: L
agents: [06-trading-domain-specialist]
adrs: [ADR-080, ADR-073, ADR-069, ADR-083]
waivers: []
status: done
opened: 2026-10-27
closed: 2026-10-27
---

## Реплика

Ретроспективный разбор (IN-019/ADR-086, Этап 8, п.2) — первый worked example
из брифа: «пара ADR-080 → CALC-CE-ARBITRAGEUR-BASIS: решение отменило
предпосылку карточки (ноги зеркальны), и разрыв проявился на экране
«базисом» −14,5 тыс. USDT».

## Runtime (для реплик по экрану)

Не применимо — это не наблюдение на живом стенде, а ретроспективный разбор
уже устранённого расхождения (панель исправлена 2026-10-20, карточка
обновлена 2026-10-20/22, до начала этой сессии ADR-086/IN-019).

## Анализ (подъём)

| Р | Артефакт | Что сказано | Совпадает |
|---|---|---|---|
| Р8 | `docs/frontend/ui-map.yaml#UI-VC-POS-BASIS` | До CR: `capability: CAP-F18-NN` (placeholder) | Не описано — ни одна из 3 существовавших capabilities[] F-18 не владела этим элементом (known-gaps.md, запись Этап 7) |
| Р4 | `docs/04-domain/calculations/CALC-F18-CE-ARBITRAGEUR-BASIS.md` | Формула §2, числовые примеры §3, явная секция «Открытые вопросы и расхождения» уже описывают ADR-080/ADR-073 конфликт и его исправление (2026-10-20/22) | Да — карточка математически верна и актуальна (проверено `06-trading-domain-specialist` независимо, см. «Проверка»); но нет структурного frontmatter-блока `examples:` (IN-019 §4.5) — машиночитаемого примера, который можно прогнать раннером |
| Р2 | `feature.yaml.capabilities[]` | До CR: ни одна из 3 capabilities (`CAP-F18-BAND-WIDTH`, `CAP-F18-SYSTEM-PNL`, `CAP-F18-LINK-TRANSFER`) не ссылается на `CALC-F18-CE-ARBITRAGEUR-BASIS` или `UI-VC-POS-BASIS` | Не описано — у поведения «базис/открытая позиция арбитражёра» не было ни capability, ни формального AC, хотя код/тесты/документация давно существуют (`status: implemented` с 2026-10-20) |

Верхняя точка — Р2 (возможность не заведена), не Р4 (формула верна, менять
нечего) и не Р8 (экран верен, менять нечего).

## Спуск (левая ветвь)

| Р | Статус | Файлы / причина |
|---|---|---|
| Р2 | изменено | `feature.yaml`: добавлена `capabilities[].id: CAP-F18-ARBITRAGEUR-BASIS` + `acceptanceCriteria: [AC-F18v2-29]` (новый AC, ретроактивно называет уже реализованное поведение — текст проверен `trading-domain-specialist`, учтена оговорка про `CE_ARB_MARKET_HEDGE` и реверс ADR-083) |
| Р2 | изменено | `docs/02-system/use-cases/UC-F18-04-.../use-case.md`: добавлено поле `capabilities: [...]` (все 4 capability F-18, ранее поле отсутствовало вовсе) |
| Р4 | изменено | `CALC-F18-CE-ARBITRAGEUR-BASIS.md`: добавлен `examples:` блок (EX-1, зеркальная позиция ETH, `expect: {open_position_usdt: 0, basis_usdt: 0.246106}`, `tolerance: 0.000001` — точное значение и допуск проверены `trading-domain-specialist` пересчётом, не 0,246 из округлённой прозы карточки). Формула/текст §1–7 карточки НЕ изменены — уже верны |
| Р8 | изменено | `docs/frontend/ui-map.yaml#UI-VC-POS-BASIS`: `capability: CAP-F18-NN` → `CAP-F18-ARBITRAGEUR-BASIS` |
| Р5 | untouched | `docs/05-components/sequences/SEQ-F18-UC-F18-04-services.md` — не входит в сужённый объём этого CR (решение владельца, Этап 8); путь панели «Живые позиции» (BFF `buildAgentRows` → фронт) не покрыт текущей service-sequence диаграммой такта клиринга — отдельная задача, см. known-gaps.md |
| Р12 | untouched | Код (`arbPositionParts`, `buildAgentRows`) не менялся — поведение уже корректно, CR закрывает только документацию/реестр |

## Проверка (правая ветвь)

| Р | Проверка | Результат |
|---|---|---|
| Р2 | `consistency/check.py --scope F-18` (C-02: AC/UC резолвятся, ID уникален) | Зелёно для `CAP-F18-ARBITRAGEUR-BASIS` (см. запуск ниже) |
| Р4 | Исполняемый пример EX-1 | **Не прогнан автоматически** — `tools/calc-examples/` (раннер, IN-019 §4.5) не существует, не строится в этом CR (см. known-gaps.md). Проверено вручную: `trading-domain-specialist` пересчитал формулу независимо (0,246106, не 0,246); я перепроверил в Python (`(a+b)*(ps+pd)/2`, `((b-a)/2)*(pd-ps)` на тех же числах) — сошлось до 1e-13 |
| Р8 | `consistency/check.py --scope F-18` (C-09: ui↔capability сверка) | **not-yet-applicable** — C-09 требует OpenAPI BFF (Этап 3 п.3, не построен), правило не проверяет автоматически |
| — | `code-reviewer` | Не вызывался — нет триггера `.claude/rules/agent-routing.md` §2 (ни один money/contract/schema путь не затронут: только `feature.yaml`, `use-case.md`, CALC-frontmatter, `ui-map.yaml`, `tools/consistency/check.py`) |

`consistency/check.py --scope F-18` после CR: 11 error / 8 warn (не 0 —
остальные находки Этапов 2/4 по F-18 не входят в объём этого CR, см.
known-gaps.md "Этап 8"). Полная приёмка Этапа 8 (`--scope F-18 без ошибок`)
**не достигнута этим CR** — он закрывает только разрыв по
`CAP-F18-ARBITRAGEUR-BASIS`, не весь F-18.

## Решения

- Новая capability `CAP-F18-ARBITRAGEUR-BASIS`, `since: CR-F18-001`.
- Новый AC `AC-F18v2-29` — ретроактивно называет уже реализованное и
  задокументированное поведение (не меняет его).
- `examples:` в CALC-карточке — числа проверены независимо
  (`trading-domain-specialist` + ручной пересчёт), НЕ прогнаны раннером
  (раннера нет).
- Остальные 21 ADR из группы ADR-064…085, экраны `/ce-graph-live` и
  `/ce-treasury-live`, свёртка 9 планов F-18 в 1 — вне объёма этого CR,
  см. `docs/00-methodology/known-gaps.md` "Этап 8".

## План (для XL)

Не применимо — класс L, не XL.
