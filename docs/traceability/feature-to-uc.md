# Trace Matrix: Feature → Use Cases

> **Ось:** функциональная декомпозиция (IN-013).
> **Уровни:** L0 Feature ☁️ → L1 Use Cases 🌊.
>
> Эта матрица показывает: **какие use cases раскрывают каждую Feature**.
> Парная матрица — [`uc-to-sequences.md`](uc-to-sequences.md) (UC → L0/L1/L2 sequences).
>
> Полная методология двухосевой декомпозиции —
> [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../00-methodology/functional-hierarchy-and-decomposition.md).

## Текущее покрытие

| Feature (L0 ☁️) | Имя | Use Cases (L1 🌊) |
| --- | --- | --- |
| F-01 | Auth & Identity | UC-F01-01 Authenticate User |
| F-02 | Create FlowOrder | UC-F02-01 Create Flow Order |
| F-03 | Order Lifecycle | UC-F03-01 Amend / Cancel Order |
| F-04 | Batch Clearing Cycle | UC-F04-01 Run Batch Clearing |
| F-05 | Live Market Data | UC-F05-01 Stream Market Data |
| F-06 | Positions / PnL / Margin | UC-F06-01 Show Positions |
| F-07 | Pre-trade Risk | UC-F07-01 Pre-trade Risk Check |
| F-08 | Post-trade Risk / Liquidations | UC-F08-01 Liquidate Position |
| F-09 | Batch / Combo / Multi-leg Orders | UC-F09-01 Create Combo Order; UC-F09-02 Grouped Matching; UC-F09-03 External-Leg Execution |
| F-10 | MM Liquidity Curves | UC-F10-01 Publish MM Curve |
| F-11 | External Venues / LOB → FOB | UC-F11-01 Onboard Venue; UC-F11-02 Publish Snapshot; UC-F11-03 Build Liquidity Curve; UC-F11-04 Venue Health Degradation; UC-F11-05 Execute Hedge On Venue; UC-F11-01 Ingest External Market Data |
| F-12 | Execution Hedge | UC-F12-01 Auto Hedge After Batch; UC-F12-02 Manual Operator Hedge; UC-F12-03 Partial Fill Retry; UC-F12-04 Rejection Fallback; UC-F12-05 Timeout / Underfilled Reconciliation |
| F-13 | Post-trade Report | UC-F13-01 Generate Post-trade Report |
| F-14 | Deposit / Withdraw | UC-F14-01 Deposit Funds |
| F-15 | Backtest / Replay | UC-F15-01 Create Replay Session; UC-F15-01 Replay Historical Batch; UC-F15-02 Cancel Replay Session; UC-F15-03 A/B Compare Sessions; UC-F15-04 Audit Mode Replay; UC-F15-05 Retry Failed Session; UC-F15-06 Replay Determinism Check |
| F-16 | Operator Console | UC-F16-01 Trigger Kill Switch |
| F-17 | Monitoring & Alerts | UC-F17-01 Fire Alert |
| F-20 | Live Venue Simulator | (UC TBD — `02-system/use-cases/UC-F20-*` пока planned) |

## Notes

- **F-15** имеет два разных UC с id `UC-F15-01` (create-replay-session
  и replay-historical-batch); это исторический slug-collision —
  flagged в [`coverage-matrix.md`](coverage-matrix.md) и в feature-yaml-checker.
- **F-11** имеет дополнительные UC-ID-naming variants
  (`SEQ-F11-NN-name-services.md` vs `SEQ-F11-UC-F11-NN-services.md`) —
  legacy от IN-004; не блокирует ingest, но при ревизии унифицировать.
- **F-20** — feature.yaml зарегистрирован, UC и L0/L1 sequences пока
  deferred (post-MVP, статус `planned`).

## Связанные матрицы

| Матрица | Ось | Покрытие |
| --- | --- | --- |
| [feature-to-uc.md](feature-to-uc.md) (этот файл) | Функционал | F-XX → UCs |
| [uc-to-sequences.md](uc-to-sequences.md) | Декомпозиция | UC → L0/L1/L2 sequences |
| [sequence-to-code.md](sequence-to-code.md) | Реализация | L2 sequence → cpp/ files |
| [coverage-matrix.md](coverage-matrix.md) (existing) | Этапы | Feature → 01–11 stage artifacts |
| [feature-traceability.md](feature-traceability.md) (existing) | Этапы | Feature → contracts / data / tests |
| [source-to-artifact-map.md](source-to-artifact-map.md) (existing) | Источники | IN-NNN → target docs |
