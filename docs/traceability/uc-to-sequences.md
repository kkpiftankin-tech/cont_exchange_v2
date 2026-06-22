# Trace Matrix: Use Case → Sequences (L0 / L1 / L2)

> **Ось:** уровневая декомпозиция (IN-013).
> **Уровни:** L1 Use Case 🌊 → L0 System ☁️ + L1 Service 🌊 + L2 Component-internal 🐟.
>
> Эта матрица показывает: **какие sequence-диаграммы раскрывают каждый Use Case**
> на трёх уровнях декомпозиции.
>
> Полная методология — [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../00-methodology/functional-hierarchy-and-decomposition.md).

## Mapping convention

| Уровень | Расположение | Frontmatter `level:` |
| --- | --- | --- |
| L0 ☁️ System | `docs/02-system/use-cases/{UC-ID}/sequences/SEQ-{UC-ID}-system.md` | `kite` |
| L1 🌊 Service | `docs/05-components/sequences/SEQ-{F-ID}-{UC-ID}-services.md` | `sea` |
| L2 🐟 Component-internal | `docs/05-components/{component}/sequences/SEQ-{COMPONENT}-NNN-{topic}.md` | `fish` |

## Текущее покрытие (sequence inventory)

> **Авто-генерация**: пока вручную. См. open question OQ-IN013-01
> ниже про автоматический generator из `find docs/`.

| UC | L0 System ☁️ | L1 Service 🌊 | L2 Component 🐟 (count) |
| --- | --- | --- | --- |
| UC-F01-01 | `02-system/use-cases/UC-F01-01-*/sequences/` | `SEQ-F01-UC-F01-01-services.md` | (none yet) |
| UC-F02-01 | `…/UC-F02-01-create-flow-order/sequences/` | `SEQ-F02-UC-F02-01-services.md` | (none yet) |
| UC-F03-01 | `…/UC-F03-01-amend-cancel-order/sequences/` | `SEQ-F03-UC-F03-01-services.md` | (none yet) |
| UC-F04-01 | `…/UC-F04-01-run-batch-clearing/sequences/` | `SEQ-F04-UC-F04-01-services.md` | `cpp/matching/src/domain/solver_impl.cpp` (L2 sequences планируются) |
| UC-F05-01 | `…/UC-F05-01-stream-market-data/sequences/` | `SEQ-F05-UC-F05-01-services.md` | (none yet) |
| UC-F06-01 | `…/UC-F06-01-show-positions/sequences/` | `SEQ-F06-UC-F06-01-services.md` | (none yet) |
| UC-F07-01 | `…/UC-F07-01-pretrade-risk-check/sequences/` | `SEQ-F07-UC-F07-01-services.md` | (none yet) |
| UC-F08-01 | `…/UC-F08-01-liquidate-position/sequences/` | `SEQ-F08-UC-F08-01-services.md` | (none yet) |
| UC-F09-01 | `…/UC-F09-01-create-combo-order/sequences/SEQ-UC-F09-01-system.md` | `SEQ-F09-UC-F09-01-services.md` | `cpp/matching/src/domain/grouped_solver_bisection.cpp`, `multileg_feasible_caps.cpp` (L2 sequences планируются) |
| UC-F09-02 | `…/UC-F09-02-grouped-matching/sequences/` | `SEQ-F09-UC-F09-02-services.md` | (none yet) |
| UC-F09-03 | `…/UC-F09-03-external-leg-execution/sequences/` | `SEQ-F09-UC-F09-03-services.md` | (none yet) |
| UC-F10-01 | `…/UC-F10-01-publish-mm-curve/sequences/` | `SEQ-F10-UC-F10-01-services.md` | (none yet) |
| UC-F11-01 (onboard) | `…/UC-F11-01-onboard-venue/sequences/` | `SEQ-F11-01-onboard-venue-services.md` | (none yet) |
| UC-F11-01 (ingest) | `…/UC-F11-01-ingest-external-marketdata/sequences/` | `SEQ-F11-UC-F11-01-services.md` | (none yet) |
| UC-F11-02 | `…/UC-F11-02-publish-snapshot/sequences/` | `SEQ-F11-02-publish-snapshot-services.md` | `cpp/venues/src/infra/cex_ws_rest_adapter.cpp` (L2 sequences планируются) |
| UC-F11-03 | `…/UC-F11-03-build-liquidity-curve/sequences/` | `SEQ-F11-03-build-curve-services.md` | `cpp/venues/src/domain/depth_curve_builder.cpp` (L2 sequences планируются) |
| UC-F11-04 | `…/UC-F11-04-venue-health-degradation/sequences/` | `SEQ-F11-04-health-routing-services.md` | (none yet) |
| UC-F11-05 | `…/UC-F11-05-execute-hedge-on-venue/sequences/` | `SEQ-F11-05-execute-on-venue-services.md` | (none yet) |
| UC-F12-01 | `…/UC-F12-01-auto-hedge-after-batch/sequences/` | `SEQ-F12-01-auto-hedge-services.md` | `cpp/matching/src/app/hedge_trigger_policy.cpp`, `execution_planner.cpp` (L2 sequences планируются) |
| UC-F12-02 | `…/UC-F12-02-manual-operator-hedge/sequences/` | (TBD) | (none yet) |
| UC-F12-03 | `…/UC-F12-03-partial-fill-retry/sequences/` | (TBD) | (none yet) |
| UC-F12-04 | `…/UC-F12-04-rejection-fallback/sequences/` | `SEQ-F12-02-rejection-fallback-services.md` | (none yet) |
| UC-F12-05 | `…/UC-F12-05-timeout-underfilled-reconciliation/sequences/` | `SEQ-F12-03-error-scenarios-services.md` | (none yet) |
| UC-F13-01 | `…/UC-F13-01-generate-posttrade-report/sequences/` | `SEQ-F13-UC-F13-01-services.md` | (none yet) |
| UC-F14-01 | `…/UC-F14-01-deposit-funds/sequences/` | `SEQ-F14-UC-F14-01-services.md` | (none yet) |
| UC-F15-01 (create) | `…/UC-F15-01-create-replay-session/sequences/` | `SEQ-F15-01-create-session-services.md` | `cpp/backtest/src/app/run_replay_session_uc.cpp` (L2 sequences планируются) |
| UC-F15-01 (replay) | `…/UC-F15-01-replay-historical-batch/sequences/` | `SEQ-F15-02-replay-cycle-services.md` | (none yet) |
| UC-F15-02 | `…/UC-F15-02-cancel-replay-session/sequences/` | (TBD) | (none yet) |
| UC-F15-03 | `…/UC-F15-03-ab-compare-sessions/sequences/` | (TBD) | (none yet) |
| UC-F15-04 | `…/UC-F15-04-audit-mode-replay/sequences/` | (TBD) | (none yet) |
| UC-F15-05 | `…/UC-F15-05-retry-failed-session/sequences/` | (TBD) | (none yet) |
| UC-F15-06 | `…/UC-F15-06-replay-determinism-check/sequences/` | (TBD) | (none yet) |
| UC-F16-01 | `…/UC-F16-01-trigger-kill-switch/sequences/` | (TBD) | (none yet) |
| UC-F17-01 | `…/UC-F17-01-fire-alert/sequences/` | (TBD) | (none yet) |

## L2 Component-internal sequences (Fish 🐟)

L2 sequences по компонентам — пока сосредоточены в нескольких сервисах:

| Component | L2 sequences dir | Файлы |
| --- | --- | --- |
| matching | `docs/05-components/matching-fob-core/sequences/` | (см. directory listing) |
| matching | `docs/05-components/matching/sequences/` (когда появится) | — |
| venues | `docs/05-components/external-venues-connector/sequences/` | (см. directory) |
| venues | `docs/05-components/venue-liquidity-curve-builder/sequences/` | (см. directory) |
| ledger | `docs/05-components/ledger/sequences/` (когда появится) | — |
| order_flow | `docs/05-components/order-flow/sequences/` (когда появится) | — |

→ Систематический набор L2 sequences — open backlog. Появляется по
мере детальной разработки новых features или рефакторинга существующих.

## Open questions

| OQ | Описание | Tracked in |
| --- | --- | --- |
| OQ-IN013-01 | Автоматический generator inventory: `find docs/02-system/use-cases/*/sequences/` + `docs/05-components/sequences/` + frontmatter parse → автоматическое обновление этой матрицы | post-IN-013 backlog |
| OQ-IN013-02 | UC-ID slug-collision (F-15 имеет два UC-F15-01) — fix через rename одного UC или адаптация конвенции | F-15 backlog |
| OQ-IN013-03 | L2 sequences систематически отсутствуют для большинства компонентов — backlog для будущего PR | — |

## Связанные матрицы

| Матрица | Ось | Что показывает |
| --- | --- | --- |
| [feature-to-uc.md](feature-to-uc.md) | Функционал | F-XX → UCs |
| [uc-to-sequences.md](uc-to-sequences.md) (этот) | Декомпозиция | UC → L0/L1/L2 sequences |
| [sequence-to-code.md](sequence-to-code.md) | Реализация | L2 sequence → `cpp/` files |
| [coverage-matrix.md](coverage-matrix.md) | Этапы | Feature → 01–11 stage artifacts |
