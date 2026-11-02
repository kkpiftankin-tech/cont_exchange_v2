# Профиль dev — 2026-10-12 (этап 0, freeze base)

Снимок **реальных** CE-флагов на dev-стенде (`docker exec infra-ledger-1 env`).
Нужен для воспроизводимости: dev читает `infra/env/.env-example` напрямую, но
значения на стенде могли расходиться с git (см. причину 6 разбора). Это
зафиксированная база прогона [EXP-2026-10-12-baseline](EXP-2026-10-12-baseline.md).

## CE-флаги (ledger, авторитетный полный набор)

| Флаг | Значение | Флаг | Значение |
|---|---|---|---|
| CE_AGENTS_ENABLED | 1 | CE_AGENT_POS | 1 |
| CE_AGENT_BAND | 1 | CE_V2_GRAPH | 1 |
| CE_CROSS_PAIRS | **1** ⚠ (разбор предполагал 0) | CE_NUMERAIRE | USDT |
| CE_AGENT_BAND_Q_TRANSLATOR | 18 | CE_AGENT_BAND_Q_ARBITRAGEUR | 30 |
| CE_BAND_GAMMA_MODE | 1 | CE_BAND_SIGMA_REF | 0.00015 |
| CE_BAND_HEDGE_COOLDOWN_MS | 2000 | CE_BAND_MAKER_DEADLINE_MS | 30000 |
| CE_BAND_MAKER_RESERVE_ENABLED | **0** (резидентный maker выкл) | CE_DEAD_ZONE_HALF_SPREAD_MULT | 0 |
| CE_DEAD_ZONE_WIDE_SPREAD_PM | 20 | CE_MIN_DEPTH_RATIO | 0.01 |
| VENUES_SIM_MATCH_REAL_TRADES | 1 | VENUES_SIM_TRADE_WINDOW_MS | 10000 |
| VENUES_SIMULATE_ORDERS | 1 | CE_HEDGE_ALLOWED_VENUES | binance,okx |
| CE_HEDGE_MODE | FLATTEN | CE_HEDGE_MARGIN_BPS | 1 |
| CE_NET_HEDGE_ENABLED | 0 | CE_HOUSE_VENUE | (пусто) |

## Seed капитала (единая книга holdings)

BTC=10 · ETH=100 · SOL=1000 · USDT=1 000 000.

## Параметры рёбер (α / c по активам)

- STOCK α: BTC/ETH/SOL=8, USDT=60; c: BTC/ETH/SOL=0.15, USDT=0.02.
- TRANSFER α: BTC/ETH/SOL=12, USDT=18; c: BTC/ETH/SOL=0.25, USDT=0.05.

## Замечания
- `CE_CROSS_PAIRS=1` — кросс-пары (ETH/BTC, SOL/BTC) активны на dev, вопреки
  предположению разбора (там CE_CROSS_PAIRS=0). Учесть при анализе.
- `CE_BAND_MAKER_RESERVE_ENABLED=0` подтверждает: «maker-зона» на деле тейкерская
  (причина 1, этап 3).
- `CE_HEDGE_ALLOWED_VENUES=binance,okx` относится к legacy stock-hedge пути, не к
  band-хеджу агентов (band филлится и на kraken/coinbase — см. baseline).
- Полный git-профиль с `config_version`-историей `f05a_clearing_config` —
  следующий шаг этапа 0 (devops).
