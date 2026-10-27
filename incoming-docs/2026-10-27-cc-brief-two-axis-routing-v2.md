---
id: CC-BRIEF-2026-10-26-two-axis-routing
version: 2
title: Двухосевая маршрутизация реплик, V-петля «спецификация — проверка» и согласованность проекта
audience: Claude Code (исполнитель), владелец проекта (приёмка)
repo: kkpiftankin-tech/cont_exchange_v2
base: feat/F-18-ce-treasury-nop @ 6fa63ee (2026-10-23); сверено с b5dd45a (2026-10-26)
status: proposed
companion: учебник для владельца «Реплика сквозь уровни» (интерактивная HTML-страница)
supersedes: version 1 (2026-10-26, утро)
---

# Инструкция для Claude Code: маршрутизация реплик, V-петля, согласованность

## Что изменилось против версии 1

| Было в v1 | Стало в v2 | Почему |
|---|---|---|
| Путь реплики: подъём → спуск «до кода и ниже» по линии Р1…Р15 | V-петля: спуск по левой ветви (спецификация → код), затем подъём по правой ветви (проверка каждого затронутого уровня) | V-модель: у каждого уровня спецификации есть своя проверка; «ниже кода» — это проверки, а не продолжение линии |
| Уровни как линия | Уровни как граф зависимостей; спуск — топологический порядок затронутых артефактов, независимые ветви — параллельно | экран (Р8) и данные (Р7) зависят от контракта (Р6), но не друг от друга |
| «Весь проект согласован» как цель | Три класса артефактов: нормативные (блокируют), поясняющие (дата сверки, предупреждения), генерируемые (не правятся руками) | полная ручная согласованность недостижима и порождает обход правил |
| REST-документ руками для каждого пути BFF | OpenAPI как нормативный контракт BFF для путей с экранов; инвентарь маршрутов генерируется из `server.js`; md-документ объясняет смысл | ~84 пути руками не поддержать; схема проверяется машинно |
| Карта экрана руками для всех страниц | Скелет карты генерируется из `data-ui-id` и вызовов API; руками — смысл, единицы, CALC, возможность | то же |
| CR-файл на каждое изменение | Класс S — git-трейлер в коммите; файл CR — для M, L, XL | лёгкие изменения не должны тонуть в бумагах |
| Stop-хук блокирует код без CR | Блок с явным выходом: трейлер `Change: S` или `Change: spike` с причиной | гейт без выхода начинают обходить |
| Docs-first для всего | Docs-first для денежного контура, контрактов и данных; исследовательская дорожка (spike → решение → спецификация) для экспериментов F-18 | исследование и производство требуют разной строгости |
| Числовые примеры CALC — текст | Примеры CALC и критерии приёмки — исполняемые тесты | исполняемая спецификация не расходится с кодом молча |
| Подъём начинается с документов | Для реплик по экрану сначала runtime: свежая ли сборка на стенде, свежи ли данные | частая причина «неверного числа» — старый бинарник или зависшая цепочка |
| OpenAPI BFF в `frontend/api/openapi.yaml` | `contracts/openapi/fob/bff/v1/api/bff.yaml` + `schemas/bff.json` | в проекте уже есть OpenAPI hedge и replay по схеме `contracts/openapi/fob/<сервис>/v1/`; правило `contracts.md` (paths: `contracts/**`) подгрузится само |
| Решение — ADR-085 | Решение — ADR-086 | номер 085 занят 26.10 (капитальный предел переводчика) |
| ID для критериев приёмки — новая работа | У F-18 ID уже есть (`AC-F18v2-01…27`, 28 шт.), у F-09 — 10; возможности группируют существующие ID, ID не меняются; ID нужны остальным 17 фичам | сверка `feature.yaml` 26.10; ни один тест пока не ссылается на ID |
| `specs/waivers.yaml` с нуля | Обобщает уже работающие исключения в описании PR: «traceability: waived (reason: …)» (`traceability-gate`) и «drift-waiver:» (`docs-code-drift`) | исключение должно быть видно в репозитории и иметь владельца и срок |

## 0. Как выполнять эту инструкцию

1. Файл приходит вложением в чат: хук `UserPromptSubmit` положит его в `incoming-docs/`. Зарегистрируй его как `IN-NNN` навыком `ingest-docs` (meta + fragment-map).
2. Выполняй этапы раздела 8 по порядку. Один этап — одна короткая ветка и один PR.
3. В начале этапа покажи план (файлы, проверки, агенты) и жди подтверждения владельца.
4. Не меняй поведение `cpp/` и `frontend/` сверх перечисленного явно: атрибуты `data-ui-id`, валидация ответов BFF по схеме в тестах, якоря-комментарии.
5. Существующие ID (F-XX, BR-*, AC-*, UC-*, ADR-*, CALC-*, EXP-*) не переименовывай. Исключение — дубли UC (этап 1).
8. Цифры состояния проекта, на которые опирается инструкция, и команды их пересчёта — в приложении А. Перед этапом пересчитай нужные: ветка живая.
6. Каждый этап закрывается «Критериями приёмки» и отчётом по `CLAUDE.md` §5.
7. Конфликт этой инструкции с документами проекта — Conflict Note и вопрос владельцу.

## 1. Принципы (обязательны при выполнении всех этапов)

| ID | Принцип | Как применяется здесь | Источник |
|---|---|---|---|
| P-01 | Один писатель на изменение | Главная сессия правит все уровни; агенты проверяют, исследуют, дают черновик | Anthropic «Building effective agents»; Cognition «Don't Build Multi-Agents» |
| P-02 | Подагент — для изоляции контекста, параллельной независимой работы и независимой проверки | Навигатор подъёма, профильная проверка уровня, ревью денежного контура | Claude Code docs «Subagents»; Anthropic «Effective context engineering» |
| P-03 | Обязательное — детерминированно | Всё, что должно происходить всегда, — хук, `permissions` или CI. `CLAUDE.md` и правила — контекст, не принуждение | Claude Code docs «Memory» |
| P-04 | Короткие непротиворечивые инструкции | `CLAUDE.md` < 200 строк; противоречащие инструкции удаляются — модель может выбрать любую из них; аудит `/doctor prompt-audit` | Claude Code docs «Memory» |
| P-05 | Единственный источник правды для каждого факта | Связи — в `feature.yaml` и frontmatter; индексы и матрицы генерируются | Docs as Code; Living Documentation |
| P-06 | Исполняемая спецификация | Числовые примеры CALC и критерии приёмки — тесты | Specification by Example |
| P-07 | V-петля и двусторонняя трассировка | Каждому затронутому уровню спецификации — своя проверка; изменение закрыто, когда закрыты проверки | V-модель; ISO/IEC/IEEE 29148 |
| P-08 | Контракт первым и машиночитаемым | Proto — с `buf breaking`; REST BFF — OpenAPI; md-документ объясняет смысл, не копирует схему | OpenAPI Specification; buf |
| P-09 | Маленькие изменения | Короткие ветки, одна мысль на коммит или PR, частое слияние. Сейчас обратное: ветка F-18 — 191 коммит поверх `main` в одном PR №48 (последнее слияние в `main` — 27.08); коммит 596b170 — три решения, 55 файлов, +7 257 строк | Trunk-based development; Google «Small CLs»; DORA |
| P-10 | Исследование отдельно от производства | Эксперимент за флагом и карточка `EXP-*`; спецификация — после решения | Feature Toggles (Hodgson, martinfowler.com) |
| P-11 | Наблюдаемость прежде объяснения | Число на экране несёт происхождение: CALC, входы, время расчёта, версия сборки | практики наблюдаемости и data lineage (OpenLineage) |
| P-12 | ADR — для архитектурно значимых решений | Остальные решения владельца — в разделе «Решения» CR | Nygard «Documenting Architecture Decisions» |
| P-13 | Гейт с правом исключения | Любое правило согласованности допускает waiver с причиной, владельцем и сроком | fitness functions (Ford, Parsons, Kua) |
| P-14 | Маршрутизатор измеряется | Эталонный набор реплик (eval) прогоняется до и после изменений маршрутизатора | Anthropic «Building effective agents» (routing) |
| P-15 | Runtime first для реплик по экрану | Сначала версия сборки на стенде, свежесть данных, верный маршрут; потом документы | практика диагностики; `rebuild-service` в проекте |

## 2. Термины

| Термин | Определение |
|---|---|
| Реплика | Сообщение владельца: вопрос о системе, замечание, пожелание. Чаще всего — по наблюдению на фронте. |
| Ось Ф | Где в продукте: `F-XX` → `CAP-FXX-NN` → `UC-FXX-NN` (шаг) → `UI-…`. |
| Ось Р | На каком уровне: Р1…Р15 (раздел 3.2). Левая ветвь — спецификация и код, правая — проверка. |
| Возможность | Именованная единица функционала внутри фичи: свои AC, UC, CALC, контракты, таблицы, UI, код, тесты. |
| Элемент экрана | Поле, колонка, график, кнопка, баннер. Стабильный `UI-ID` = атрибут `data-ui-id`. |
| Якорь | Точка, где реплика касается системы. |
| Верхняя точка | Самый верхний уровень, который реплика меняет или где найдено расхождение. Не обязательно Р1. |
| Затронутое множество | Артефакты, которые достигаются из верхней точки по рёбрам реестра и меняются. |
| CR | Карточка изменения: адрес, анализ, спуск, проверка, решения. Файл — для M/L/XL, git-трейлер — для S. |
| Нормативный артефакт | Определяет поведение и проверяется машинно: AC, CALC (формула и примеры), proto, OpenAPI, схема БД, ui-map. Расхождение блокирует. |
| Поясняющий артефакт | Объясняет: README фич, обзоры компонентов, L2-последовательности, ADR-контекст. Несёт `verified:` (дата сверки); устаревание — предупреждение. |
| Генерируемый артефакт | Индексы, матрицы трассировки, инвентари маршрутов и таблиц, читаемые проекции. Руками не правится. |
| Waiver | Явное исключение из правила: правило, область, причина, владелец, срок. |
| Spike | Исследовательское изменение за флагом с гипотезой и карточкой `EXP-*`. |

## 3. Модель (нормативная часть)

### 3.1. Ось Ф

```
F-XX (фича, level: kite)
 └─ CAP-FXX-NN (возможность)            ← блок capabilities в feature.yaml
     ├─ UC-FXX-NN (+ шаг / поток)       ← level: sea
     ├─ CALC-* (вычисления)             ← L3, с исполняемыми примерами
     └─ UI-<SCREEN>-<ELEMENT>           ← docs/frontend/ui-map.yaml
F-XXA (дочерняя фича, parent: F-XX)     ← свои акторы и UC
```

Приращение без новых акторов и UC — возможность внутри фичи; иначе — дочерняя фича.

### 3.2. Ось Р: левая ветвь (спецификация) и правая (проверка)

| Р | Уровень (левая ветвь) | Артефакты | Агент-специалист | Проверка на правой ветви |
|---|---|---|---|---|
| Р1 | Бизнес-требование | `docs/01-business/`, `*-requirements.md` | `01-business-analyst` | приёмка владельцем на экране (Р15) |
| Р2 | Возможность, AC, сценарий | `feature.yaml` (`capabilities`, AC), `acceptance-criteria.md`, `use-case.md`, системная SEQ | `02-system-analyst` | приёмочные тесты по AC (AC → тест) |
| Р3 | Архитектура | C4, ADR | `03-solution-architect` | архитектурные проверки (границы, направления зависимостей) |
| Р4 | Домен | `business-rules.md`, `CALC-*`, `invariants.yaml` | `06-trading-domain-specialist` | исполняемые примеры CALC, инвариант-тесты |
| Р5 | Взаимодействие сервисов | сервисные SEQ, binding-таблицы | `02-system-analyst`, `08-cpp-service-architect` | интеграционные и e2e-тесты (`Testing/`) |
| Р6 | Контракты и DTO | `contracts/proto/`, `contracts/openapi/` (в т. ч. `fob/bff/v1/`), `docs/06-api/` | `04-proto-contract-designer` | `buf breaking`, валидация ответов BFF по OpenAPI, контрактные тесты |
| Р7 | Данные | `docs/07-data/`, `init.sql` | `05-data-schema-designer` | миграция на копии dev-тома, сверка инвентаря таблиц |
| Р8 | Экран (спецификация) | `docs/frontend/ui-map.yaml`, спецификации экранов | `07-frontend-architect` | UI-тесты (`__tests__`), соответствие `data-ui-id` карте |
| Р9 | Устройство сервиса | обзоры компонентов, `docs/09-implementation/` | `08-cpp-service-architect` | проверки слоёв (`domain` не зависит от `infra`) |
| Р10 | Тест-план | раздел «Проверка» CR, `docs/10-testing/` | `09-test-architect` | — (это проект правой ветви) |
| Р11 | Задачи | раздел «План» CR, `implementation-plan/` (XL) | `10-implementation-planner` | — |
| Р12 | Код и unit-тесты | `cpp/`, `frontend/`, `infra/` | главная сессия; `11-code-implementer`; `14-devops-engineer` | unit-тесты |
| Р13 | Ревью | — | `12-code-reviewer`, `15-security-reviewer` | вердикт ревью |
| Р14 | Сборка и стенд | CI, навык `rebuild-service` | `14-devops-engineer` | зелёный CI, сервис жив на dev |
| Р15 | Экран (наблюдение) | dev-стенд | главная сессия + владелец | значение на экране = пример CALC / AC; подтверждение владельца |

Номер агента — имя, а не позиция на оси: `06` работает на Р4, `07` — на Р8.
`00-sdlc-coordinator` — навигатор анализа (читает цепочку, возвращает таблицу); сейчас его `description` начинается с «Use only for class XL», это меняется в разделе 7. `13-docs-ingestion-engineer` — только входящие документы.
У 13 агентов из 16 нет `Edit`/`Write` и стоит `permissionMode: plan`; писать могут `11` (в `isolation: worktree`), `13` и `14`. Это и есть P-01, сохраняй.

### 3.3. Граф зависимостей уровней

```
            ┌→ Р3 ─┐
Р1 → Р2 ────┤      ├→ Р5 → Р6 ─┬→ Р7 → Р9 → Р12 (сервисы)
            └→ Р4 ─┘           └→ Р8 ──────→ Р12 (фронт, BFF)
Р10, Р11 — планы, прикладываются к CR на любом шаге после Р2.
```

Спуск идёт в топологическом порядке затронутых артефактов, не строго по номерам. Независимые ветви можно вести параллельно: Р3 и Р4 после Р2, Р7 и Р8 после Р6.

### 3.4. Типы реплик

| Тип | Признак | Типичная верхняя точка | Результат |
|---|---|---|---|
| `ask` | «что значит / откуда / почему 0» | уровень, где величина определена (Р4 или Р2) | ответ с цепочкой; CR не нужна, если цепочка полная |
| `fix` | «число неверное», «не обновляется» | уровень, чьё правило нарушено | код под документ; проверка правой ветви |
| `refine` | «на экране верно, но не описано» | уровень без описания | документы; код не меняется |
| `change` | «должно считаться иначе» | Р2 или Р4 | полный V-проход по затронутым уровням |
| `extend` | «добавь колонку / режим / график» | Р8 (данные есть в API), Р6 (нет), Р4 (новое вычисление), Р2 (новая возможность) | новая или расширенная CAP |
| `new` | функции нет в реестре | Р1 | XL |
| `describe` | «опиши, как работает X» | Р2 | документы по коду |
| `spike` | «попробуй / проверь гипотезу» | — | эксперимент за флагом + `EXP-*`; после решения — CR `extend`/`change` |

### 3.5. Алгоритм обработки реплики

1. **Тип и якорь.** Тип по 3.4. Якорь: для экрана — маршрут, вкладка, подпись или `data-ui-id` → элемент `ui-map.yaml`; иначе `tools/route/route.py --candidates`. Неоднозначно — один вопрос владельцу.
2. **Runtime first** (для `ask`/`fix` по экрану). Сверь: версию сборки на стенде с HEAD ветки, свежесть данных (индикаторы liveness), маршрут. Причина в окружении → чинить окружение (`rebuild-service`), CR не нужна или `fix` на Р14.
3. **Горизонтальный переход.** От наблюдения (правая ветвь) к его спецификации (левая): `ui-map` → OpenAPI/контракт → CALC/AC.
4. **Анализ (подъём).** `route.py --ascent <якорь>` → цепочка вверх. На каждом уровне строка таблицы «Анализ»: `уровень | артефакт | что сказано | совпадает (да / нет / не описано)`. Длинная цепочка → `00-sdlc-coordinator` (ответ до 30 строк).
5. **Верхняя точка.** Самый верхний уровень с «нет» или «не описано», либо уровень, где пожелание меняет смысл. Для `ask` с полной цепочкой — ответ и конец.
6. **Согласование.** L и XL: формулировка изменения на верхнем уровне в 1–2 фразах, подтверждение владельца. S и M — сообщить и продолжать.
7. **Спуск (левая ветвь).** `route.py --descent <верхняя точка>` → затронутое множество в топологическом порядке. Для каждого артефакта: изменить или `untouched` с причиной. Нетривиальное изменение специалистского уровня (Р3, Р4, Р6, Р7, Р8) — проверка профильного агента. Перед переходом к зависимому уровню — `tools/consistency/check.py --scope <CAP> --levels Рk,Рm`.
8. **Проверка (правая ветвь).** Для каждого изменённого уровня — его проверка из 3.2. Изменение закрыто, когда все проверки затронутых уровней зелёные.
9. **Р13–Р15.** Ревью (`code-reviewer` обязателен для денежного контура и класса L), `rebuild-service`, сверка значения на экране с примером CALC / AC и подтверждение владельца.
10. **Закрытие.** `tools/registry/build.py`, `tools/consistency/check.py --scope <CAP>`, CR `status: done` (или трейлер для S), отчёт по `CLAUDE.md` §5.

### 3.6. Класс изменения выводится

| Класс | Условие | Оформление |
|---|---|---|
| S | изменены только Р12–Р15; нормативные документы уже описывают поведение | трейлеры коммита `Change: S`, `Refs: <CAP или UI>` |
| M | верхняя точка Р2 или Р8; Р3, Р4, Р6, Р7 не изменены | файл CR |
| L | изменён хотя бы один из Р3, Р4, Р6, Р7 | файл CR; профильные агенты; `code-reviewer` |
| XL | `new` или дочерняя фича | файл CR; полный проход; план задач |
| spike | исследование | `EXP-*` + трейлер `Change: spike`, флаг выключен по умолчанию |

Триггеры эскалации текущего `agent-routing.md` §2 сохраняются поверх класса.

## 4. Форматы артефактов

Номера в примерах (CAP, AC, FR, UI, CR) условные. Поля и смысл — нормативные.

### 4.1. Карта экрана — `docs/frontend/ui-map.yaml`

```yaml
screens:
  - id: UI-VC
    route: /vector-clearing-live            # как в frontend/web/src/App.js
    title: Векторный клиринг (live)
    component: frontend/web/src/pages/VectorClearing/VectorClearingLive.js
    features: [F-18, F-05A]
    elements:
      - id: UI-VC-POS-BASIS                 # = data-ui-id
        generated: {component: "VectorClearingLive.js#AgentPositionsTables", api: "GET /api/vector-clearing/agent-positions"}
        label: «Живые позиции» → арбитражёр → «базис»
        kind: value                         # value | column | chart | series | control | banner | table
        meaning: Разница цен бирж × перевозимый объём
        unit: USDT
        field: "agentRows[].basis_mtm_usdt"
        openapi: "contracts/openapi/fob/bff/v1/api/bff.yaml#/paths/~1api~1vector-clearing~1agent-positions/get"
        contract: docs/06-api/grpc/ledger-agent-positions.md
        calc: CALC-CE-ARBITRAGEUR-BASIS
        capability: CAP-F18-NN
        refresh: poll 2s
        states: {empty: "—", stale: "баннер «ЦЕПОЧКА ОСТАНОВЛЕНА» (dataAgeMs > порога)"}
```

- Блок `generated` пишет генератор (`tools/ui-map/scan.py`): находит `data-ui-id` и вызовы API в компоненте. Остальные поля заполняются руками.
- Атрибут `data-ui-id` ставится рядом с существующими `data-testid`, не вместо них.
- Поле `ui` в CALC-карточках → список UI-ID. `feature.yaml.uxScreens` → ссылки на экраны ui-map (сейчас у F-18 указан несуществующий `/ce-agents-live`).

### 4.2. OpenAPI для BFF — `contracts/openapi/fob/bff/v1/`

- Раскладка как у существующих `contracts/openapi/fob/hedge/v1/` и `fob/replay/v1/`: `api/bff.yaml` (пути) + `schemas/bff.json` (схемы). Правило `.claude/rules/contracts.md` уже покрывает `contracts/**`: дополни его разделом про OpenAPI BFF.
- Нормативный контракт для путей, на которые ссылается ui-map. Начать с путей экранов F-18.
- `tools/bff-routes/scan.py` строит инвентарь путей из `server.js` (сейчас ~84; маршрутизация написана вручную через `pathname ===`). C-07 сверяет инвентарь с OpenAPI.
- Тесты BFF проверяют форму ответов по схеме (валидатор JSON Schema).
- `docs/06-api/rest/*.md` — поясняющие: смысл полей, единицы, ссылки на CALC. Схему не копируют.

### 4.3. Proto

- Добавить `buf.yaml` для `contracts/proto/` и проверку `buf breaking` против `main` в CI.
- `docs/06-api/grpc/*.md` — поясняющие; нормативна схема `.proto`.

### 4.4. Возможности и статус в `feature.yaml`

```yaml
feature:
  id: F-18
  name: CE Virtual Counterparties
  level: kite
  lifecycle: active                         # planned | active | deprecated
  status: {docs: partial, code: partial, runtime: dev}
parentRequirements: [FR-CE-001]
capabilities:
  - id: CAP-F18-07
    title: Ширина полосы хеджа — ручной и волатильностный режимы
    status: {docs: complete, code: done, runtime: dev}
    since: CR-F18-012
    acceptanceCriteria: [AC-F18v2-21]       # ссылки на существующие ID; текст критерия остаётся в общем списке acceptanceCriteria
    adrs: [ADR-081, ADR-066]
    ucs: [UC-F18-04#A3]
    calcs: [CALC-CE-SKEW-BAND-CALIBRATION]
    contracts: ["contracts/openapi/fob/bff/v1/api/bff.yaml#/paths/~1api~1vector-clearing~1config"]
    tables: [f05a_clearing_config, operator_audit_log]
    ui: [UI-VC-BAND-MODE]
    codePaths: [cpp/ledger/src/app/ledger_uc.cpp, frontend/api/band_config.js]
    tests: [cpp/ledger/tests/ce_band_manual_test.cpp, frontend/api/band_config.test.js]
```

`feature.yaml` — индекс состояния (цель до ~200 строк). Формат — JSON Schema `specs/schemas/feature.schema.json`.

Критерии в `acceptanceCriteria` сейчас — строки вида `"AC-F18v2-21 (ADR-081): …"`. Формат строк не меняй: ID извлекается регулярным выражением `^AC-[A-Za-z0-9]+-\d+[a-z]?`. Возможность ссылается на ID. Файл `traceability.yaml` фичи (у F-18 последний раз менялся 15.09 и сообщает «все codePath — planned (код ещё не написан)») становится генерируемым из `capabilities` (этап 2).

### 4.5. Исполняемые примеры CALC

```yaml
# во frontmatter CALC-CE-ARBITRAGEUR-BASIS — пример «зеркальная позиция» из §3 карточки
examples:
  - id: EX-1
    target: frontend/api/trade_stats_math.js#arbPositionParts   # arbPositionParts(asset, numeraire, qSrc, pSrc, qDst, pDst)
    args: [ETH, USDT, 1.7579, 2398.80, -1.7579, 2398.66]
    expect: {open_position_usdt: 0, basis_usdt: 0.246}
    tolerance: 0.001
```

- Раннер `tools/calc-examples/` вызывает `target` (JS — через node; C++ — через data-driven gtest) и сравнивает с `expect`.
- Правило C-05: у CALC со статусом `implemented` есть хотя бы один пример, и он проходит.

### 4.6. Критерии приёмки → тесты

- Каждый AC имеет ID. У F-18 и F-09 ID уже есть; остальным фичам ID присваиваются при касании. Тест ссылается на AC в имени или комментарии (`AC-F18v2-21`). Сейчас ни один тест на ID не ссылается.
- Генерируется матрица `docs/traceability/ac-to-tests.md`. Правило C-17: у AC возможности с `code: done` есть хотя бы один тест.

### 4.7. Шапка use case

```yaml
---
id: UC-F18-04
title: Такт клиринга виртуальных контрагентов и хедж по полосе ±q
level: sea
feature: F-18
capabilities: [CAP-F18-01, CAP-F18-07]
supersedes: [UC-F18-01, UC-F18-02, UC-F18-03]
system-sequence: sequences/SEQ-UC-F18-04-system.md
service-sequence: ../../../05-components/sequences/SEQ-F18-UC-F18-04-services.md
status: active                              # draft | active | superseded
verified: 2026-10-26                        # дата последней сверки с кодом
---
```

### 4.8. Карточка изменения — `docs/changes/CR-FXX-NNN-<slug>.md` (M, L, XL)

```markdown
---
id: CR-F18-012
title: Ширина полосы хеджа — ручной и волатильностный режимы
type: extend                       # ask | fix | refine | change | extend | new | describe
source: ui                         # ui | chat | IN-NNN | review
anchor: UI-VC-BAND-MODE
feature: F-18
capability: CAP-F18-07
top: Р2
class: L
agents: [06-trading-domain-specialist, 05-data-schema-designer, 12-code-reviewer]
adrs: [ADR-081]
waivers: []
status: done                       # open | in-progress | done | rejected
opened: 2026-10-19
closed: 2026-10-19
---

## Реплика
## Runtime (для реплик по экрану)
## Анализ (подъём)
| Р | Артефакт | Что сказано | Совпадает |
## Спуск (левая ветвь)
| Р | Статус | Файлы / причина |
## Проверка (правая ветвь)
| Р | Проверка | Результат |
## Решения
## План (для XL)
```

### 4.9. Трейлеры коммита (класс S и spike)

```
fix(F-18): подпись колонки «базис» — единицы USDT

Change: S
Refs: UI-VC-POS-BASIS
Covered-by: CALC-CE-ARBITRAGEUR-BASIS
```

Формат сообщения — Conventional Commits (проект уже их использует); трейлеры — в подвале сообщения.

### 4.10. Исключения — `specs/waivers.yaml`

```yaml
- rule: C-07
  scope: "GET /api/venues/*"
  reason: Экраны venues переносятся в этапе 3b
  owner: gennady
  expires: 2026-12-01
```

CI падает на просроченном исключении.

Существующие исключения в описании PR («traceability: waived (reason: …)» для `traceability-gate`, «drift-waiver:» для `docs-code-drift`) продолжают работать. Проверка требует, чтобы у каждого такого исключения была запись в `waivers.yaml` с `owner` и `expires`; без записи — предупреждение в первые 4 недели, затем ошибка.

### 4.11. Реестр — `specs/registry/registry.yaml` (генерируется)

- Узлы: `requirement`, `feature`, `capability`, `ac`, `uc`, `seq`, `adr`, `calc`, `example`, `contract`, `openapi-path`, `table`, `ui-screen`, `ui-element`, `code`, `test`, `cr`, `exp`, `waiver`.
- Рёбра: `requirement→feature`, `feature→capability`, `capability→{ac,uc,calc,contract,table,ui-element,code,test,adr}`, `ac→test`, `calc→example→code`, `ui-element→{openapi-path,contract,calc}`, `uc→seq`, `seq→contract`, `cr→*`.
- У каждого узла: `level`, `class` (`normative | explanatory | generated`), `path`, `verified` (если есть).
- Генерируемые проекции: `feature-index.md`, `docs/traceability/*.md`, `docs/changes/index.md`, `docs/frontend/ui-map.md`, `ac-to-tests.md`. В каждом — строка «сгенерировано, не править».
- `specs/domain/feature-component-map.yaml` и `specs/domain/traceability.yaml` удаляются; инструменты переводятся на реестр.

## 5. Правила согласованности (fitness functions)

`tools/consistency/check.py`: `--rule`, `--levels`, `--scope`, `--format md|json`. Код 1 — при ошибке без действующего waiver.

| ID | Пара уровней | Правило | Класс |
|---|---|---|---|
| C-01 | Р1↔Р2 | у фичи непустой `parentRequirements`; ID существуют | warn → error после этапа 8 |
| C-02 | Р2 | у возможности есть AC с ID и хотя бы один UC; ID уникальны | error |
| C-03 | Р2↔Р3 | ADR из `capabilities[].adrs` существуют и ссылаются обратно | warn |
| C-04 | Р2↔Р5 | у UC `active` есть системная и сервисная SEQ | error |
| C-05 | Р4 | CALC: валидная шапка, статус из перечня, якоря `code`/`tests` существуют, у `implemented` есть проходящий пример | error |
| C-06 | Р5↔Р6 | нет открытых `TODO contract` у возможностей с `code: done` | error |
| C-07 | Р6↔Р12 | инвентарь путей BFF ⊆ OpenAPI для путей из ui-map; `buf breaking` зелёный | error |
| C-08 | Р7↔Р12 | инвентарь таблиц `init.sql` = таблицы в `docs/07-data/` | error |
| C-09 | Р8↔Р6/Р4 | у элемента ui-map есть `field` и `openapi`; у вычисляемого — `calc`; `data-ui-id` в коде = ID в карте | error |
| C-10 | Р8↔Р2 | маршруты ui-map существуют в `App.js`; экраны ссылаются на существующие фичи | error |
| C-11 | Р12↔Р2 | `codePaths` существуют; изменённые файлы входят в `codePaths` возможности из CR/трейлера | error / warn |
| C-12 | все | свежесть: поясняющий артефакт с `verified` старше последнего изменения его кода | warn |
| C-13 | CR | у CR `done` заполнены анализ, спуск и проверка; файлы существуют | error |
| C-14 | все | статусы только из перечней | error |
| C-15 | все | ID уникальны (UC, CAP, AC, CR, UI); ID в шапке = имени файла (сейчас расходится у CALC-CE-ARBITRAGEUR-BASIS) | error |
| C-16 | waivers | нет просроченных исключений | error |
| C-17 | Р2↔Р12 | у AC возможности с `code: done` есть тест | warn → error после этапа 8 |

## 6. Маршрутизатор

### 6.1. `tools/route/route.py`

- `--candidates` (stdin: текст реплики) → JSON: предполагаемый тип по `tools/route/markers.yaml`, якоря-кандидаты (UI, CAP, UC, CALC, F) с оценкой и совпавшими словами. Без LLM, < 200 мс.
- `--ascent <ID>`, `--descent <ID>` → цепочки по реестру с уровнями; `--descent` — в топологическом порядке.
- `--runtime` → версия сборки на стенде, HEAD, возраст данных по liveness-эндпоинтам (если стенд доступен).

### 6.2. Хук `.claude/hooks/route_hint.py` (`UserPromptSubmit`)

- Второй обработчик после `auto-archive-attachments.py`.
- При оценке выше порога печатает `{"hookSpecificOutput": {"hookEventName": "UserPromptSubmit", "additionalContext": "…"}}` до 1 500 символов: тип, до 5 якорей, факт «изменения оформляются навыком change».
- Утверждения, не команды. Никогда не блокирует промпт.

### 6.3. Навык `.claude/skills/change/SKILL.md`

- Вызывается моделью и командой `/change`. `description` начинается с главного: «Обрабатывает реплику о системе — вопрос, замечание по экрану, изменение, новую функцию — по V-петле: анализ до верхней точки, спуск по затронутым уровням, проверка каждого уровня». `when_to_use`: типичные фразы с фронта.
- Тело — алгоритм 3.5 со ссылками на `route.py`, `consistency/check.py`, шаблоны CR и трейлеров.
- `/triage` — обёртка: читает `docs/feedback/inbox.md`, для каждой строки запускает алгоритм с `source: ui`.

### 6.4. Правило `.claude/rules/change-routing.md`

Заменяет `agent-routing.md`. Без `paths`, до 80 строк: оси, типы реплик, алгоритм в 10 строк, вывод класса, триггеры эскалации, ссылка на навык.

### 6.5. Эталонный набор — `tools/route/golden.yaml`

20 реплик по коммитам F-18 с 2026-09-15 (не менее 10 — по экрану): ожидаемые тип, якорь, верхняя точка, класс. Прогон — `python3 tools/route/eval.py`.

## 7. Агенты

- `description` каждого агента: что делает, уровень Р, `Use when` (CR отмечает уровень changed), `Not for`, `Returns` (до 30 строк).
- Тела: вход (id CR, уровень, файлы из `--descent`), чек-лист уровня, правила C-NN уровня, формат ответа. Ссылки на удалённые разделы `CLAUDE.md` убрать (сейчас их 43).
- `00-sdlc-coordinator` — навигатор анализа: вход — якорь и цепочка `--ascent`; выход — таблица «Анализ». Без инструмента `Agent`. Текущее `description` («Plans the order of SDLC artifacts for a NEW feature… Use only for class XL») заменить: «Навигатор анализа для L и XL: проходит цепочку от якоря до верхней точки и возвращает таблицу Анализ. Use when цепочка длиннее пяти артефактов».
- Инструменты агентов не расширять: 13 из 16 работают только на чтение, и это нужно для P-01.
- `09-test-architect` — проект правой ветви для L/XL: какие проверки закрывают какие уровни.
- `13-docs-ingestion-engineer` — только `incoming-docs/`.
- Предзагрузка навыков (`skills:`): `04` ← `register-kafka-topic`; `05` ← `register-pg-table`.
- `env` в `.claude/settings.json`: `"CLAUDE_CODE_FORK_SUBAGENT": "0"` — главная сессия ждёт результат профильной проверки перед следующим уровнем.
- `rebuild-service`: `disable-model-invocation: true`.
- После 4 недель журнала `agents.jsonl` — пересмотреть состав: агенты без вызовов объединить или убрать.
- Диагностика загрузки инструкций: хук `InstructionsLoaded` пишет в `.claude/logs/instructions.jsonl`, какие `CLAUDE.md` и правила загружены, когда и почему. Приёмка этапа 6 включает выборку из журнала: `change-routing.md` грузится в каждой сессии, `cpp.md` — только после касания `cpp/**`.

Образец описания:

```yaml
---
name: proto-contract-designer
description: Уровень Р6 — проектирует и проверяет proto-сообщения, gRPC-методы, Kafka-топики и OpenAPI BFF, включая обратную совместимость (buf breaking). Use when карточка CR отмечает Р6 как changed. Not for экраны, SQL-схемы, бизнес-логика. Returns план изменения контракта, вердикт совместимости, целевые файлы (до 30 строк).
tools: Read, Grep, Glob, Bash
model: sonnet
maxTurns: 30
skills: [register-kafka-topic]
---
```

## 8. Этапы работ

### Этап 0. Решение и единый процесс

1. ADR-086 «Двухосевая маршрутизация реплик; V-петля; карточка CR; отмена сэндвича ingress → агент → ingress». Номер 085 занят (ADR-085, 26.10); перед созданием проверь, что 086 свободен.
2. `docs/00-methodology/feature-development-process.md` — переписать под эту модель (единственный процессный документ).
3. `ingest-and-agents-integration.md` → `status: superseded by ADR-086`; оставить только приём входящих документов.
4. Удалить раздел «Agent Integration (per-level sandwich)» из `.claude/skills/ingest-docs/SKILL.md`.
5. `docs/00-methodology/agents.md`: агенты лежат в `.claude/agents/`; таблица 3.2.
6. `CLAUDE.md` §3: ссылка на `change-routing.md` и навык `change`.

Приёмка: `grep -rE "сэндвич|ingress\(close\)|writer-of-record" docs .claude` находит только superseded-документы и ADR-086; `/doctor prompt-audit` не находит противоречий между `CLAUDE.md`, правилами и навыками.

### Этап 1. Гигиена реестра

1. Миграция шапок 46 UC (4.7); HTML-комментарий удалить.
2. Перенумеровать дубли `UC-F11-01` и `UC-F15-01`; в старом месте — строка-указатель; ссылки обновить.
3. Статусы фич — по 4.4 (сейчас 11 значений); статусы CALC — `implemented | partial | planned | drift | proposed` (у 7 из 14 сейчас свободный текст; пояснения — в тело).
4. F-05A → `docs/02-system/features/F-05A-vectorized-external-liquidity/` с `parent: F-05`.
5. ID требований → `FR-<ОБЛАСТЬ>-NNN`; `parentRequirements` у всех фич (неизвестное — список вопросов владельцу).
6. Долги проверок: `feature-yaml-checker` (4), `in013-checker` (31), `link-audit` (36), 43 ссылки на удалённые разделы `CLAUDE.md`; ID CALC в шапке = имени файла.
7. Битые ссылки на документы из кода и SQL: например, `init.sql` ссылается на `docs/07-data/vector-flow-segments.md`, которого нет. Найти все такие ссылки скриптом и включить в `link-audit`.

Приёмка: все валидаторы `tools/` — код 0; frontmatter всех UC читается YAML-парсером.

### Этап 2. Реестр, классы артефактов, проверка согласованности

1. `tools/registry/build.py` (4.11); проекции; удаление дублирующих YAML. `traceability.yaml` в папках фич — проекция реестра с пометкой «сгенерировано, не править».
2. Поле `class` (normative / explanatory / generated) и `verified` для поясняющих.
3. `tools/consistency/check.py` (раздел 5); `specs/waivers.yaml`.
4. `tools/level-drift` для C-12.
5. Цели в `Makefile.docs`: `registry`, `consistency`, `level-drift`.

Приёмка: повторная генерация без изменений (`git diff --exit-code`); все `error` либо исправлены, либо покрыты waiver со сроком.

### Этап 3. Экран (Р8) и контракт BFF (Р6)

1. `tools/ui-map/scan.py` + `docs/frontend/ui-map.yaml` (4.1). Порядок: `/vector-clearing-live`, `/ce-graph-live`, `/ce-treasury-live`, затем `/venues`, `/hedge-flows-live`, `/hedge-pnl-live`, затем остальные.
2. `data-ui-id` на элементах из ui-map.
3. `tools/bff-routes/scan.py` + `contracts/openapi/fob/bff/v1/` (4.2) для путей экранов F-18; первым — `GET /api/vector-clearing/agent-positions`, вторым — `GET`/`POST /api/vector-clearing/config`; валидация ответов в тестах BFF.
4. `buf.yaml` + `buf breaking` в CI (4.3).
5. CALC.`ui` → UI-ID; `feature.yaml.uxScreens` → экраны ui-map; исправить `/ce-agents-live`. Сейчас 4 карточки (CE-CLEARING-SURPLUS, CE-LINK-TRANSFER, CE-SKEW-BAND-CALIBRATION, CE-TRANSLATOR-PNL) ссылаются свободным текстом на несуществующий маршрут `/vector-clearing`, у 3 (CE-ANCHOR, CE-SYSTEM-PNL, F05A-DEPTH-WINDOW) привязки к экрану нет.
6. `07-frontend-architect.md`: работать с `ui-map.yaml` (убрать ссылки на несуществующие `ui-map.md`, `customer-ui.md`).
7. Происхождение числа на экране (P-11): для элементов с `calc` — всплывающая подсказка «формула (CALC-ID), время расчёта, версия сборки». Только отображение уже имеющихся данных; новые поля API — через CR.

Приёмка: C-07, C-09, C-10 проходят для экранов F-18; `route.py --ascent UI-VC-POS-BASIS` даёт непрерывную цепочку до Р2.

### Этап 4. Возможности, CR, исполняемые проверки

1. Блок `capabilities` (4.4) + JSON Schema + проверка в `feature-yaml-checker`.
2. `docs/changes/` + шаблон CR (4.8) + индекс; трейлеры (4.9) и PR-шаблон с полем CR/трейлера.
3. Раннер исполняемых примеров CALC (4.5); примеры для CALC F-18 со статусом `implemented`.
4. Матрица AC → тесты (4.6): в тестах F-18 проставить ссылки на существующие `AC-F18v2-NN`; ID для AC других фич — при касании.
5. `docs/feedback/README.md`: замечания с фронта — CR с `source: ui`.
6. Граница ADR ↔ CR (P-12).

Приёмка: C-02, C-05, C-13, C-14, C-15, C-17 (warn) проходят для F-18.

### Этап 5. Маршрутизатор

1. `route.py`, `markers.yaml`, `eval.py`, `golden.yaml` (раздел 6).
2. Хук `route_hint.py`.
3. Навык `change`; `/triage` — обёртка.
4. Правило `change-routing.md`; `agent-routing.md` удалить после переноса триггеров.

Приёмка: на `golden.yaml` якорь ≥ 18/20, верхняя точка ≥ 16/20, класс ≥ 16/20.

### Этап 6. Агенты под уровни

Раздел 7 целиком.

Приёмка: в `.claude/logs/agents.jsonl` за неделю агенты Р3–Р8 вызываются только в CR, где их уровень `changed`.

### Этап 7. Гейты

1. Stop-хук `.claude/hooks/level_gate.py`: изменённые файлы входят в `codePaths` или `ui-map.component`, а в сессии нет CR этой возможности и нет трейлера `Change: S|spike` в последнем коммите → один раз блокировать ход (код 2) с причиной. `money_path_guard.py` остаётся.
2. `scripts/hooks/post_edit_hint.py`: при правке файла из `codePaths`/ui-map — `additionalContext` с возможностью, UC, CALC и UI-элементами.
3. CI: `feature-yaml-checker`, `in013-checker`, `link-audit`, `consistency/check.py`, `buf breaking`, раннер примеров CALC, актуальность реестра — блокирующие; `level-drift` — отчёт в PR.

4. CI срабатывает на pull request. Пока вся ветка F-18 идёт одним PR №48, проверки видят только суммарный дифф. Предложи владельцу перейти на короткие ветки по CR от `main` (или от ветки F-18 до её слияния), чтобы каждый CR проходил CI отдельно.

Приёмка: PR, меняющий `codePaths` без CR и без трейлера, не проходит CI с понятным сообщением; PR с трейлером `Change: S` проходит.

### Этап 8. Пилот F-18

1. Анализ от экранов `/vector-clearing-live`, `/ce-graph-live`, `/ce-treasury-live`: для каждого элемента цепочка до Р2; разрывы — CR `refine`.
2. Ретроспективные CR `refine` по группам ADR-064…085 (22 ADR после 2026-09-15). Первым — пара ADR-080 → CALC-CE-ARBITRAGEUR-BASIS: решение отменило предпосылку карточки (ноги зеркальны), и разрыв проявился на экране «базисом» −14,5 тыс. USDT. У каждого ADR заполнить затронутое множество (возможности, CALC, UC).
3. Обновить `capabilities` F-18, `UC-F18-04`, `SEQ-F18-UC-F18-04-services`, CALC (с примерами).
4. 9 планов F-18 закрыть ссылками на CR; оставить один действующий.

Приёмка: `consistency/check.py --scope F-18` без ошибок; `level-drift --scope F-18` без устаревших нормативных уровней.

### Этап 9. Распространение и метрики

- Остальные фичи — при касании, через `describe` / `refine`. Сплошной переписки нет.
- Еженедельно: `agents_report.py`; точность маршрутизатора (`eval.py` и исправления владельца); доля коммитов с кодом, у которых есть CR или трейлер; `level-drift`; метрики DORA по git и CI — время от коммита до стенда (change lead time), частота выкладок на dev, доля неудачных выкладок (change fail rate), доля внеплановых исправляющих выкладок (deployment rework rate), время восстановления после неудачной выкладки.

## 9. Чего не делать

- Не менять бизнес-логику и вёрстку ради этой инструкции.
- Не писать документы «для полноты»: уровень без изменений — `untouched` с причиной.
- Не поднимать «истину» из кода молча: расхождение — `fix` или Conflict Note + CR `change`.
- Не копировать схему в md-документы: нормативна схема (proto, OpenAPI, SQL), документ объясняет.
- Не вызывать агентов «на всякий случай».
- Не править генерируемые файлы руками.
- Не удалять файлы без разрешения владельца: устаревшее помечается `superseded`.

## 10. Итоговая приёмка

1. Вопрос по экрану («что за число в колонке „базис“?») получает ответ с цепочкой Р15 → Р8 → Р6 → Р4 → Р2 и значением из примера CALC.
2. Изменение даёт CR с анализом, верхней точкой, спуском, проверкой правой ветви и выведенным классом; лёгкое изменение — трейлер.
3. `consistency/check.py` без ошибок (или с действующими waivers); CI блокирует нарушения.
4. F-18 согласована: нормативные артефакты сходятся с кодом, примеры CALC проходят, у AC есть тесты.

## 11. Источники

- Claude Code docs — Memory (CLAUDE.md, правила, размер, противоречия): https://code.claude.com/docs/en/memory
- Claude Code docs — Subagents: https://code.claude.com/docs/en/subagents
- Claude Code docs — Skills: https://code.claude.com/docs/en/skills
- Claude Code docs — Hooks: https://code.claude.com/docs/en/hooks
- Anthropic — Building effective agents: https://www.anthropic.com/research/building-effective-agents
- Claude Code — Best practices (Explore → Plan → Implement → Commit): https://code.claude.com/docs/en/best-practices
- Anthropic — Effective context engineering for AI agents: https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents
- Anthropic — How we built our multi-agent research system: https://www.anthropic.com/engineering/multi-agent-research-system
- Cognition — Don't Build Multi-Agents: https://cognition.com/blog/dont-build-multi-agents
- M. Nygard — Documenting Architecture Decisions: https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions
- C4 model: https://c4model.com/diagrams
- Diátaxis: https://www.diataxis.fr/start-here/
- Write the Docs — Docs as Code: https://writethedocs.org/guide/docs-as-code/
- G. Adzic — Specification by Example: https://www.manning.com/books/specification-by-example
- C. Martraire — Living Documentation (обзор): https://www.infoq.com/articles/book-review-living-documentation/
- OpenAPI Specification: https://www.openapis.org/what-is-openapi
- buf — breaking change detection: https://buf.build/docs/breaking/
- Pact — contract testing: https://docs.pact.io/
- Trunk-based development: https://trunkbaseddevelopment.com/
- Google Engineering Practices — Small CLs: https://google.github.io/eng-practices/review/developer/small-cls.html
- DORA metrics: https://dora.dev/guides/dora-metrics/
- P. Hodgson — Feature Toggles: https://martinfowler.com/articles/feature-toggles.html
- Conventional Commits: https://www.conventionalcommits.org/en/v1.0.0/
- OpenLineage: https://openlineage.io/docs/
- Building Evolutionary Architectures (fitness functions): https://evolutionaryarchitecture.com/precis.html
- ISO/IEC/IEEE 29148 (трассируемость требований): https://www.iso.org/standard/72089.html
- arc42: https://arc42.org/overview/
- Agile Manifesto: https://agilemanifesto.org/
- V-model: https://en.wikipedia.org/wiki/V-model_(software_development)
- GitHub — Spec-driven development (Spec Kit): https://github.blog/ai-and-ml/generative-ai/spec-driven-development-with-ai-get-started-with-a-new-open-source-toolkit/
- Claude Code 2.1.302 — fork-субагенты по умолчанию (обзор Classmethod, 17.10.2026): https://dev.classmethod.jp/en/articles/20261017-cc-updates-v2-1-302/

## Приложение А. Исходные данные и как их пересчитать

Снимок 6fa63ee (23.10.2026), сверка с b5dd45a (26.10.2026). Окно истории — 190 коммитов с 27.08 по 23.10.

| Факт | Значение | Как пересчитать |
|---|---|---|
| Коммиты с кодом (`cpp/`, `frontend/` без тестов и md) | 119 из 190 (63 %) | `git log --since=2026-08-27 --until=2026-10-24 --name-only` + классификация путей |
| Из них обновили описание фичи, UC или последовательности | 7 (6 %) | то же, пути `docs/02-system/features/F-*/`, `docs/02-system/use-cases/` |
| Из них обновили хотя бы один уровень Р2–Р8 (вкл. proto, OpenAPI, `docs/06-api`, `init.sql`, `docs/07-data`, `docs/frontend`) | 32 (27 %) | то же |
| Коммиты с ADR / CALC / UC / последовательностями / спецификациями экранов | 55 / 28 / 7 / 4 / 0 | то же |
| Ветка против `main` | 191 коммит поверх `main` (26.10); последнее слияние в `main` — PR №46, 27.08; открыт PR №48 | `git rev-list --count origin/main..HEAD`; `git ls-remote origin 'refs/pull/*/head'` |
| `CLAUDE.md` | 149 строк, 11 904 байт | `wc -lc CLAUDE.md` |
| Правила без `paths` | только `agent-routing.md` (6 007 байт) | frontmatter `.claude/rules/*.md` |
| Описания агентов / навыков | 5 586 / 2 197 символов | поле `description` |
| Агенты без `Edit`/`Write` | 13 из 16; все на `model: sonnet` | frontmatter `.claude/agents/*.md` |
| ADR | 84 на 23.10, 85 на 26.10; после 15.09 — ADR-064…085 | `ls docs/03-architecture/adr` |
| Критерии приёмки с ID | F-18: 28 (`AC-F18v2-01…27`, `13a`); F-09: 10 (`AC-F09-001…010`); остальные 17 фич: 0; ссылок из тестов: 0 | `grep -o 'AC-F[0-9A-Za-z]*-[0-9]*' feature.yaml`; `grep -r AC-F18v2- --include=*.{js,cpp,py}` |
| CALC | 14 карточек, у 7 статус вне перечня шаблона; привязка к экрану (`ui:`) у 11: у 7 — маршрут, компонент, поле; у 4 — свободный текст с несуществующим маршрутом `/vector-clearing` | `grep '^status:' docs/04-domain/calculations/CALC-*.md`; блок `ui:` во frontmatter |
| OpenAPI | есть у hedge и replay (`contracts/openapi/fob/*/v1/`); у BFF нет | `find contracts/openapi -type f` |
| Пути BFF | ~84: 67 точных `pathname === …` и около 17 префиксных и regex-маршрутов (`startsWith`, `match`) в `frontend/api/server.js`; документов REST — 13 | `grep -o 'pathname === "[^"]*"' frontend/api/server.js \| sort -u`; `ls docs/06-api/rest` |
| Фронт | 23 каталога страниц в `frontend/web/src/pages/`, 31 маршрут в `App.js`, 101 `data-testid`; `docs/frontend/specs/` — один документ (F16) | `ls frontend/web/src/pages`; `grep -o 'path="[^"]*"' frontend/web/src/App.js`; `grep -r data-testid frontend/web/src` |
| Таблицы Postgres без описания | 6 из 41: `combo_compensations`, `execution_reports_raw`, `f05a_clearing_config_history`, `replay_fee_models`, `replay_reward_configs`, `sim_hedge_pnl` | `CREATE TABLE` в `infra/postgres/init.sql` против `docs/07-data/` |
| CI | 8 сценариев на pull request; блокируют `traceability-checker`, `proto-contract-auditor` (только учёт proto в `proto-map.yaml`) и проверка ссылок; `docs-code-drift` не блокирует; `feature-yaml-checker` не запускается | `.github/workflows/*.yml` |
