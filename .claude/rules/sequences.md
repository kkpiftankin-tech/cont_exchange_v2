---
paths: ["docs/**/sequences/**", "docs/02-system/use-cases/**", "docs/05-components/**"]
---

# Sequence diagram placement rules (перенесено из CLAUDE.md §26a)

Каждая sequence-диаграмма имеет **уровень декомпозиции** (IN-013, Cockburn).
Полное описание — [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../docs/00-methodology/functional-hierarchy-and-decomposition.md).

1. **L0 ☁️ System-level sequence diagrams** (внешний участник ↔ Continuous
   Exchange System как black box) хранятся **только** в:

   ```text
   docs/02-system/use-cases/{UC-ID}/sequences/SEQ-{UC-ID}-system.md
   ```

   Frontmatter: `level: kite`. Участники: только actors + `[System]`.
   Запрещены имена внутренних сервисов (`matching`, `risk`, ...).

2. **L1 🌊 Service-level sequence diagrams** (cross-component, e.g. `API
   Gateway → Matching → Risk → Kafka → Ledger`) хранятся **только** в:

   ```text
   docs/05-components/sequences/SEQ-{F-ID}-{UC-ID}-services.md
   ```

   Frontmatter: `level: sea`. Участники: только компоненты верхнего уровня +
   Kafka topics + PG/CH tables. Запрещены имена классов/методов внутри
   компонента.

3. **L2 🐟 Component-internal sequence diagrams** (внутренняя
   последовательность одного сервиса) хранятся **только** в:

   ```text
   docs/05-components/{component-name}/sequences/SEQ-{COMPONENT}-NNN-{topic}.md
   ```

   Frontmatter: `level: fish`. Участники: только модули/классы ЭТОГО
   компонента. Чужие компоненты — `participant X as X [external]` без
   раскрытия.

4. Каждая sequence-диаграмма обязана ссылаться на:
   - Feature (`docs/02-system/features/`)
   - Use Case (`docs/02-system/use-cases/`)
   - Related Components (`docs/05-components/`)
   - Related Contracts (`docs/06-api/`)
   - Related Data Objects (`docs/07-data/`)

   Ссылки размещаются **ВНЕ** mermaid-блока, в виде markdown-таблицы или
   секции `## Трассировка`. Markdown-ссылки `[text](url)` внутри mermaid
   рушат рендеринг (IN-013).

5. Каждая стрелка L1 service-level диаграммы должна иметь backing-контракт:
   REST endpoint в `docs/06-api/rest/`, gRPC method в `docs/06-api/grpc/`,
   Kafka topic в `docs/06-api/messaging/`, или SQL/DDL в `docs/07-data/`.

6. **Запрещено** генерировать код, если у фичи нет: Feature-файла (L0),
   Use Case-файла (L1), L0 System-level и L1 Service-level sequence diagram,
   контрактов в `docs/06-api/`, data schema в `docs/07-data/`.

7. **Запрещено** размещать L0 system-level диаграммы вне
   `02-system/use-cases/`, L1 service-level — вне `05-components/sequences/`,
   L2 component-internal — вне `05-components/{component}/sequences/`. Любое
   нарушение — fail в traceability.

Уровень L3 «вычисление» (карточки `docs/04-domain/calculations/CALC-*`,
формула → код → тест → экран) лежит ещё на ступень ниже L2 и не является
sequence-диаграммой — см. `docs/04-domain/calculations/README.md`.
