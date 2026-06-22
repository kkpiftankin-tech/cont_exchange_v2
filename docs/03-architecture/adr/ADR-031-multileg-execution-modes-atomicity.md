---
id: ADR-031
status: accepted
date: 2026-06-05
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - docs/03-architecture/adr/ADR-022-batch-clearing-solver-replay.md
  - docs/03-architecture/adr/ADR-025-risk-manager-boundaries.md
  - docs/03-architecture/adr/ADR-032-parent-child-order-model.md
  - docs/06-api/grpc/order-flow-create-combo-order.md
  - contracts/proto/fob/orders/v1/combo.proto
  - incoming-docs/IN-011.meta.md
  - CLAUDE.md (§3.3 ADR, §15 matching rules)
source: IN-011 (F-09 v2 corrected §2, §3, §10, §21)
---

# ADR-031: Режимы исполнения и атомарность многоногих заявок (F-09)

## Контекст

Текущая документация F-09 ([order-flow-create-combo-order.md](../../06-api/grpc/order-flow-create-combo-order.md),
[UC-F09-01](../../02-system/use-cases/UC-F09-01-create-combo-order/use-case.md))
описывает combo как «атомарный набор FlowOrder, который исполняется как единое
целое — либо все ноги, либо отмена». Это смешивает два принципиально разных
режима и обещает гарантию атомарности, недостижимую для внешних площадок без
native multi-leg support.

Источник IN-011 (F-09 v2 corrected) явно требует **различать** два режима и
**не** маскировать оркестрацию независимых ног под атомарное исполнение
(Conflict CN-IN011-01, CN-IN011-04). Молча выбрать одну версию нельзя
(CLAUDE.md §0a Conflict rule).

## Решение

Зафиксировать **два режима исполнения** (`executionMode`) и **пять политик
атомарности** (`atomicityPolicy`) как часть контракта F-09. Режим и политика
проходят сквозь весь chain: `order_flow → risk → matching → venues → ledger`.

### Режимы исполнения

- **`orchestration_only`** — система создаёт parent object для группы заявок
  (интерфейс, отчётность, отмена группы, статусы), но **не гарантирует**
  математически согласованное исполнение. Каждая нога живёт как независимая
  FlowOrder. Целевые weights / ratio / spread / portfolio exposure **могут
  отклоняться**. Допустим только при явном выборе пользователя и обязан
  показывать предупреждение.
- **`multileg_vector_solver`** — настоящее многоногое исполнение. Matching
  решает grouped problem и выбирает **согласованный вектор исполнений** `e_g`
  (а не независимые объёмы ног), результат фиксируется как один
  `ExecutionGroup` ([ADR-032](ADR-032-parent-child-order-model.md),
  [ADR-033](ADR-033-execution-groups-topic.md)).

### Политики атомарности

- **`strict_atomic`** — все обязательные ноги в допустимых пропорциях или
  ничего; `orphanLegs = 0`. Если \(\alpha_g < \alpha_g^{min} \Rightarrow \alpha_g = 0\).
- **`scalable_atomic`** — согласованное исполнение с возможным уменьшением
  общего масштаба: \(e_g = \alpha_g \rho_g\), ratio/weights в пределах tolerance.
- **`best_effort`** — максимально близко к target, отклонения по ногам
  допустимы; обязательна фиксация `violatedConstraints`, `fallbackAction`,
  `ratioDeviation`, `reasonCode` и user-visible `degraded` статуса.
- **`sequential_fallback`** — последовательное исполнение **только** при явном
  разрешении пользователя / Risk / execution policy. Запрещён молчаливый
  перевод `strict_atomic`/`scalable_atomic` в последовательное исполнение.
- **`external_compensating`** — для внешних площадок без атомной multi-leg
  гарантии: best-effort + compensating action; статус группы `degraded /
  compensating / rollback_pending / rolledback`.

### atomicityScope

`internal_batch` | `venue_native` | `external_compensating` | `none` —
определяет, **где** реально обеспечивается атомарность. `strict_atomic` для
внешних ног разрешён только при `venue_native` (native atomic multi-leg) или
если все обязательные ноги исполняются внутри internal batch cycle.

### Жёсткое правило (gate)

Любая заявка, требующая сохранения weights / ratio / spread / factor exposure /
budget / leverage / margin / portfolio risk, **обязана** исполняться через
`multileg_vector_solver`. Реализовать «настоящую F-09» через набор независимых
FlowOrder с общим `parentId` нельзя — это допустимо только как
`orchestration_only` и не должно обещать multi-leg guarantees (IN-011 §21,
AC-F09-001).

## Альтернативы

- **Один режим «combo = атомарный набор FlowOrder»** (текущий стаб) — отклонено:
  обещает атомарность, недостижимую для внешних площадок; не сохраняет
  ratio/weights во времени.
- **Только `multileg_vector_solver`** — отклонено: лишает пользователя дешёвого
  режима «просто сгруппировать», нужного для UX и отмены группы.
- **Атомарность как булев флаг** — отклонено: не различает strict/scalable/
  best_effort/external, скрывает деградацию.

## Последствия

- **Плюс:** честная семантика гарантий; невозможно выдать частичное внешнее
  исполнение за атомарное; чёткая граница MVP-срезов (orchestration → scalable →
  strict → external).
- **Минус:** matching усложняется (grouped solver + atomicity postprocessor);
  risk и ledger получают grouped-ветви; растёт число статусов.
- Изменяет matching/clearing algorithm — согласовано с
  [ADR-022](ADR-022-batch-clearing-solver-replay.md): детерминизм/replay
  сохраняются для grouped execution (AC-F09-010).

## Обратимость

Низкая. `executionMode` / `atomicityPolicy` / `atomicityScope` — часть proto и
Kafka-контрактов F-09; изменение после релиза требует нового ADR и
миграционного плана. Набор алгоритмов солвера за интерфейсом — обратим, пока
соблюдены контракт и replay parity.
