<!-- manually archived 2026-06-11 — chat-attached as `<document>` block; auto-archive hook не сработал (получен mojibake-вариант) -->
<!-- original chat filename: docs-methodology-guide.md -->
<!-- type: methodology proposal / two-axis (stage × decomposition-level) documentation guide -->
<!-- декодировано latin-1 → utf-8 -->

# Руководство по документированию: функциональная иерархия и диаграммы последовательности

> **Кому адресован этот файл:** Claude Code и разработчикам проекта.
> **Что делает Claude Code с этим файлом:** при создании или изменении любого
> документа в `docs/` — следует правилам из этого руководства, самостоятельно
> определяет нужный уровень, формирует файл по шаблону и кладёт его в правильную папку.
>
> **Что НЕ делает этот файл:** не меняет существующую структуру папок.
> Все существующие файлы остаются на месте.

## Оглавление

1. Главная идея: два измерения документации
2. Три уровня функциональностей
3. Правила sequence-диаграмм по уровням
4. Ссылки между уровнями
5. Обязательные поля (frontmatter)
6. Куда класть какой файл
7. Инструкции для Claude Code
8. Запреты и типичные ошибки
9. Пример полного цикла: F-04 Batch Clearing
10. Чек-лист перед коммитом

## 1. Главная идея: два измерения документации

Документация проекта организована по **двум независимым осям**.

### Ось 1 — Этапы (папки `01-business` → `10-operations`)

Отвечает на вопрос **«когда»** это создаётся в жизненном цикле проекта.
Это уже существующая структура папок — она не меняется.

```text
01-business  →  02-system  →  03-architecture  →  04-domain
     ↘  05-components  →  06-api  →  07-data  →  09-testing  →  10-operations
```

### Ось 2 — Уровни декомпозиции (L0 / L1 / L2)

Отвечает на вопрос **«насколько глубоко»** мы смотрим внутрь системы.
Это **добавляется** к существующей структуре через правила размещения файлов.

```text
L0 — Система как чёрный ящик  (Actor → [System])
L1 — Компоненты внутри системы  (Matching → Risk → Ledger)
L2 — Классы внутри компонента  (BatchSolver → PriceCalc → ConstraintAdapter)
```

> **Почему это важно:** `domain`, `api`, `data` существуют на КАЖДОМ уровне.
> Без явного разделения непонятно — это домен всей системы или одного компонента?
> Решение: `domain`, `api`, `data` конкретного компонента живут ВНУТРИ папки этого компонента.

## 2. Три уровня функциональностей

Основа: модель уровней целей Alistair Cockburn («Writing Effective Use Cases»).
Каждая функциональность принадлежит одному из трёх уровней.

### Уровень L0 — Feature (Kite ☁️)

**Что это:** крупная бизнес-функциональность. Охватывает несколько сессий пользователя,
несколько use cases. Отвечает на вопрос «ЧТО система делает» с точки зрения бизнеса.

**Примеры:** F-04 Batch Clearing Cycle, F-07 Hedge Execution, F-12 Position Reconciliation.

**Как определить:** если на вопрос «как это работает?» можно привести несколько разных
сценариев — это Feature (L0). Она раскрывается через Use Cases ниже.

### Уровень L1 — Use Case (Sea 🌊)

**Что это:** пользовательская цель. Один актор, одно место, одно время, измеримый
результат. Именно здесь сосредоточена основная часть документации.

**Примеры:** UC-F04-01 Run Batch Clearing Cycle, UC-F04-02 Handle Batch Failure.

**Как определить:** можно задать вопрос «зачем пользователь/система это делает?» и
получить конкретный ответ. Если результат нельзя измерить — уровень неверен.

### Уровень L2 — Subfunction (Fish 🐟)

**Что это:** подфункция, которая поддерживает Use Case, но не имеет самостоятельной
ценности для пользователя. Это внутреннее поведение одного компонента.

**Примеры:** SEQ-L2-solve-batch, SEQ-L2-load-orders, SEQ-L2-get-ref-prices.

**Как определить:** если функциональность вызывается только из другой функциональности
и никогда напрямую актором — это Fish (L2).

### Итоговая таблица

| Уровень | Символ | Вопрос «как?» | Временной масштаб | Кто читает |
| --- | --- | --- | --- | --- |
| **L0 Feature** | ☁️ Kite | Порождает несколько L1 Use Cases | Дни–недели | Бизнес, архитекторы |
| **L1 Use Case** | 🌊 Sea | Порождает несколько L2 Subfunctions | Минуты | Архитекторы, тимлиды |
| **L2 Subfunction** | 🐟 Fish | Деталь реализации компонента | Секунды | Разработчики |

## 3. Правила sequence-диаграмм по уровням

> **Главное правило:** участники диаграммы определяются уровнем.
> Нарушение этого правила — самая частая ошибка.

### L0 Sequence — System Boundary

**Участники:** только внешние акторы + `[System]` как единый чёрный ящик.

**Назначение:** показать ЧТО система делает с точки зрения внешнего мира.
Внутренние детали (компоненты, методы) здесь запрещены намеренно — чтобы
диаграмма не устаревала при каждом рефакторинге.

**Размещение файла:** `docs/02-system/features/{feature-id}.md` (секция Sequence Diagram).

```text
sequenceDiagram
    actor Scheduler
    participant System as [System]

    Scheduler->>System: RunBatch(batchId)
    Note over System: step-1: Загрузка и валидация заявок
    Note over System: step-2: Получение референсных цен
    Note over System: step-3: Решение батча (solveBatch)
    Note over System: step-4: Публикация результатов
    System-->>Scheduler: BatchResult
```

После диаграммы — таблица ссылок на Use Cases (ссылки НЕ в mermaid-блоке!).

### L1 Sequence — System Internals

**Участники:** компоненты верхнего уровня системы (Matching Backend, Risk Service,
Ledger, Market Data и т.д.). Внешние акторы допустимы только как инициаторы.

**Назначение:** показать КАК система достигает цели use case — через взаимодействие
своих компонентов. Здесь фиксируются контракты (gRPC, Kafka, REST, SQL).

**Размещение файла:** `docs/03-architecture/system-sequences/SEQ-{uc-id}.md`.

После диаграммы — таблица ссылок на L2 sequences.

### L2 Sequence — Component Internals

**Участники:** модули/классы внутри ОДНОГО компонента. Другие компоненты допустимы
только как внешние blackbox-участники (без раскрытия их внутренностей).

**Назначение:** показать как конкретный компонент реализует подфункцию.
Это документация для разработчика, меняющего данный компонент.

**Размещение файла:** `docs/05-components/{component-name}/sequences/SEQ-L2-{name}.md`.

После диаграммы — секция `## Трассировка` со ссылками вверх и на код.

## 4. Ссылки между уровнями

> **Ключевое правило:** ссылки между уровнями размещаются **ВНЕ** mermaid-блока
> в виде Markdown-таблицы или секции `## Трассировка`.
> Ссылки внутри mermaid-блока (`[text](url)`) вызывают ошибки рендеринга.

### Направление ссылок

Каждый документ содержит ссылки в **обоих направлениях**:

- L0 Feature → parent-requirement (REQ-NNN) и decomposed-into (UC-XX-YY)
- L1 Use Case → parent-feature (F-NN) и l1-sequence (SEQ-UC-NN-YY)
- L1 Sequence → parent-uc и (таблица) step-N → SEQ-L2-…
- L2 Sequence → expands-step (SEQ-UC-NN-YY#step-N) и implemented-by (src/…)

### Почему двунаправленные ссылки

Это позволяет ответить на два важных вопроса:

- «Что изменится, если я изменю требование REQ-017?» — идём вниз по цепочке.
- «Почему существует этот класс BatchSolver?» — идём вверх по цепочке.

## 5. Обязательные поля (frontmatter)

Каждый документ начинается с YAML frontmatter между `---`. Это машиночитаемые
метаданные для автоматической трассировки и матриц покрытия.

### Feature (L0 / Kite ☁️)

```yaml
---
id: F-04
title: "Batch Clearing Cycle"
level: kite
parent-requirement: "REQ-017"
decomposed-into:
  - UC-F04-01
  - UC-F04-02
---
```

### Use Case (L1 / Sea 🌊)

```yaml
---
id: UC-F04-01
title: "Run Batch Clearing Cycle"
level: sea
parent-feature: "F-04"
expands-step: "все шаги F-04 L0 sequence"
l1-sequence: "../../03-architecture/system-sequences/SEQ-UC-F04-01.md"
acceptance-criteria:
  - "solveBatch возвращает BatchResult и FillEvent[]"
  - "FlowOrders загружаются из PostgreSQL"
  - "Результаты публикуются в Kafka topics batch.outputs и fills"
---
```

### L1 Sequence

```yaml
---
id: SEQ-UC-F04-01
title: "Batch Clearing — System Level Sequence"
level: sea
parent-uc: "UC-F04-01"
components-involved:
  - matching-backend
  - market-data
  - risk
  - ledger
  - kafka
---
```

### L2 Sequence (Subfunction / Fish 🐟)

```yaml
---
id: SEQ-L2-solve-batch
title: "Solve Batch — Matching Backend Internals"
level: fish
component: "matching-backend"
expands-step: "../../03-architecture/system-sequences/SEQ-UC-F04-01.md#step-3"
implemented-by:
  - "src/matching-backend/solver/BatchSolver.cpp"
  - "src/matching-backend/solver/PriceCalc.cpp"
tested-by:
  - "tests/unit/matching/test_batch_solver.cpp"
---
```

## 6. Куда класть какой файл

> Существующая структура папок **не меняется**.
> Ниже — правила для новых файлов.

```text
docs/
├── 00-methodology/
│   └── docs-methodology-guide.md
├── 02-system/
│   ├── features/{feature-id}.md          ← L0
│   └── use-cases/{uc-id}.md              ← L1
├── 03-architecture/
│   └── system-sequences/
│       └── SEQ-{uc-id}.md                ← L1 sequence
├── 05-components/{component-name}/
│   └── sequences/
│       └── SEQ-L2-{name}.md              ← L2 sequence
└── traceability/
    ├── feature-to-uc.md                  ← матрица F → UC
    ├── uc-to-sequences.md                ← матрица UC → L1+L2
    └── sequence-to-code.md               ← матрица L2 → src
```

## 7. Инструкции для Claude Code

### При создании новой Feature

1. Определить уровень: это Feature (Kite)? Охватывает несколько сессий? → L0.
2. Создать `docs/02-system/features/{feature-id}.md` с frontmatter.
3. Написать L0 sequence ТОЛЬКО с `actor` + `participant System as [System]`.
4. После mermaid-блока добавить таблицу ссылок на Use Cases.
5. Дополнить `docs/traceability/feature-to-uc.md`.

### При создании нового Use Case

1. Убедиться, что родительская Feature существует.
2. Создать `docs/02-system/use-cases/{uc-id}.md` с frontmatter.
3. Создать `docs/03-architecture/system-sequences/SEQ-{uc-id}.md`.
4. В L1 sequence: только компоненты верхнего уровня как `participant`.
5. После mermaid-блока добавить таблицу ссылок на L2 sequences.
6. Дополнить `docs/traceability/uc-to-sequences.md`.

### При создании L2 Sequence

1. Определить компонент-владелец.
2. Создать `docs/05-components/{component}/sequences/SEQ-L2-{name}.md` с frontmatter.
3. Участники: только классы/модули ЭТОГО компонента.
4. Другие компоненты — только как `participant X as X [external]`.
5. После mermaid-блока добавить секцию `## Трассировка`.
6. Дополнить `docs/traceability/sequence-to-code.md`.

## 8. Запреты и типичные ошибки

| Ситуация | Запрещено | Правильно |
| --- | --- | --- |
| L0 sequence | `participant MB as Matching Backend` | `participant System as [System]` |
| L0 sequence | `MB->>Risk: CheckLimits(...)` | `Note over System: step-3: Расчёт рисков` |
| L1 sequence | `MB->>BC: BatchController.solveBatch()` | `Note over MB: step-3: solveBatch` |
| L2 sequence | `S->>Risk: Risk.checkLimits(...)` с деталями Risk | `S->>RA: checkLimits(result)` где RA — адаптер |
| Любой уровень | `[текст ссылки](../path/to/file.md)` внутри mermaid-блока | Таблица после блока |

### Типичные ошибки

1. **Один большой sequence на всё** — смешаны L0 и L2. Разбить по правилам.
2. **domain/api/data на корневом уровне** — должны быть внутри папки компонента.
3. **Sequence без frontmatter** — добавить по шаблону из §5.
4. **Ссылки в mermaid-блоке** — вынести в таблицу после блока.

## 9. Пример полного цикла: F-04 Batch Clearing

Полный список артефактов для одной функциональности. Это эталон.

```text
REQ-017 (docs/01-business/requirements.md)
  │
  └──► F-04 Batch Clearing Cycle          [L0, kite ☁️]
      docs/02-system/features/F-04-batch-clearing.md
        │  L0 sequence: Actor → [System]
        │  Ссылки вниз: UC-F04-01, UC-F04-02
        │
        └──► UC-F04-01 Run Batch Clearing Cycle    [sea 🌊]
            docs/02-system/use-cases/UC-F04-01.md
              │
              └──► SEQ-UC-F04-01                   [L1 sequence]
                  docs/03-architecture/system-sequences/SEQ-UC-F04-01.md
                    │  Participants: SCH, MB, MDS, Risk, Ledger, Kafka
                    │
                    ├──► SEQ-L2-load-orders         [fish 🐟]
                    ├──► SEQ-L2-get-ref-prices      [fish 🐟]
                    ├──► SEQ-L2-solve-batch         [fish 🐟]
                    └──► SEQ-L2-post-fills          [fish 🐟]
```

## 10. Чек-лист перед коммитом

**Уровни и участники:**

- L0 sequence содержит только `Actor` + `[System]`.
- L1 sequence содержит только компоненты верхнего уровня.
- L2 sequence ограничена одним компонентом; чужие отмечены `[external]`.

**Фронтматтер:**

- Каждый новый Feature-файл имеет: `id`, `level: kite`, `parent-requirement`, `decomposed-into`.
- Каждый новый Use Case: `id`, `level: sea`, `parent-feature`, `l1-sequence`.
- Каждый новый L2 sequence: `id`, `level: fish`, `component`, `expands-step`, `implemented-by`.

**Ссылки:**

- Нет ссылок `[text](url)` внутри mermaid-блоков.
- После каждой L0 sequence — таблица ссылок на Use Cases.
- После каждой L1 sequence — таблица ссылок на L2 sequences.
- Каждый L2 sequence имеет секцию `## Трассировка`.

**Матрицы трассировки:**

- `docs/traceability/feature-to-uc.md` обновлён.
- `docs/traceability/uc-to-sequences.md` обновлён.
- `docs/traceability/sequence-to-code.md` обновлён.
