# F-20 + F-12 — Quickstart (запуск для проверки)

Минимальный путь: поднять стек, активировать sim-сессию, кинуть тестовый ордер,
увидеть как F-12 сформировал хедж, а симулятор реально исполнил его по живому
binance LOB.

## Что вы запускаете

`Testing/f20-demo.sh start` поднимает полный dev-стек:

| Сервис             | Зачем                                                     |
|--------------------|-----------------------------------------------------------|
| gateway            | HTTP-вход — `POST /v1/flow-orders` от трейдера            |
| order_flow         | приём FlowOrder, резерв средств, нормализация в Kafka     |
| matching           | batch-cycle, fills, F-12 hedge_trigger_policy             |
| risk, ledger       | риск-чеки, ledger applies (для live-пути и F-12 PG state) |
| venues             | подключается к **реальным** binance/coinbase/uniswap;     |
|                    | роутит intent'ы через **SimSessionRegistry** (SIM/LIVE)   |
| market_data        | history of fills/batches в ClickHouse                     |
| redpanda + console | Kafka + UI (http://localhost:8080)                        |
| postgres           | OLTP (sim_sessions, flow_orders, hedgeflows, ...)         |
| clickhouse         | OLAP (history)                                            |

Адаптеры в `venues` запущены в `multi_real` — это **живые WS+REST** к биржам.
В сети нужен доступ к binance/coinbase/Ethereum RPC. Симулятор работает на
актуальном LOB этих площадок.

## Веб-UI для проверки (PR-F20-19)

Поднимите фронт (отдельный compose):
```bash
cd frontend && docker compose up -d --build
```

Открыть в браузере (после логина `demo@cont.local` / `password123`):

| URL                                       | Что делает                                                |
|-------------------------------------------|-----------------------------------------------------------|
| http://localhost:8091/sim-sessions        | **Sim sessions** — create / list / complete (SIM_ONLY/SHADOW/LIVE_ONLY) |
| http://localhost:8091/main                | размещение FlowOrder (BUY/SELL, цены, qty)                |
| http://localhost:8091/hedge-flows-live    | F-12: hedgeflows status, filled_qty, hedge_pnl            |
| http://localhost:8091/hedge-pnl-live      | F-12 PnL аналитика по venue/symbol                        |
| http://localhost:8091/execution-live-feed-live | live execution reports (ClickHouse)                  |
| http://localhost:8091/reconciliation-alerts-live | алерты расхождения live vs sim                     |
| http://localhost:8091/policy-config-live  | F-12 hedge trigger / intent policy конфиг                 |
| http://localhost:8080                     | Redpanda Console: топики, schemas, сообщения              |

Минимальный сценарий "проверить F-12 + Sim из UI":
1. `/sim-sessions` → **Создать** сессию: name=demo, mode=SIM_ONLY, venues=binance, instruments=BTC/USDT → Активировать.
2. `/main` → разместить BUY 0.002 BTC по диапазону вокруг текущей binance цены.
3. Через ~5 сек на `/execution-live-feed-live` появится sim-исполнение; в `/hedge-flows-live` — соответствующий hedgeflow.
4. `/sim-sessions` → **Complete** для отключения sim-режима.

## Автоматический режим (опт-ин через env)

По умолчанию sim **не активируется автоматически** — оператор должен создать
SimSession (через UI/REST/CLI). Это безопасный дефолт: вы случайно не
переключите production в sim.

Если хотите чтобы venues при старте сами поднимали sim-сессию (полезно для
dev/staging — нулевой риск отправить реальный ордер), выставьте env:

```bash
VENUES_AUTO_SIM_MODE=SIM_ONLY          # SIM_ONLY | SHADOW | LIVE_ONLY
VENUES_AUTO_SIM_NAME=auto-default       # отображается в UI
VENUES_AUTO_SIM_SCOPE_VENUES=           # csv; пусто = wildcard (все venues)
VENUES_AUTO_SIM_SCOPE_INSTRUMENTS=      # csv; пусто = wildcard
VENUES_AUTO_SIM_STALE_LOB_THRESHOLD_MS=600000
```

После рестарта venues:
- Если сессии с этим именем нет — создаётся одна `auto-default` с заданными
  scope/mode. Сразу публикуется в `sim.config` → подхватывается роутером.
- Если она уже ACTIVE — оставляется как есть (нет дублей при рестартах).

Отключить authentication автоматики после запуска: `Complete` сессии через
UI (`/sim-sessions`) или REST — venues её больше не создаёт пока имя
остаётся занятым (PG `sim_sessions` PK). Чтобы заставить пере-создать —
поменяйте `VENUES_AUTO_SIM_NAME` на другое.

## Типичный demo-цикл (CLI)

```bash
# 1) Поднять (или убедиться что поднят)
bash Testing/f20-demo.sh start

# 2) Проверить статус: контейнеры, активные sim-сессии, HW топиков
bash Testing/f20-demo.sh status

# 3) Активировать SIM_ONLY для binance/BTC/USDT
bash Testing/f20-demo.sh sim-on
# вывод: sim_session_id=<uuid>

# 4) В отдельном терминале — следить за цепочкой:
bash Testing/f20-demo.sh watch

# 5) Кинуть тестовый ордер (demo-user уже с балансом)
bash Testing/f20-demo.sh order
# вывод: {"order_id":"<uuid>","accepted":true}

# 6) Через ~5 сек посмотреть, что HW sim.execution.venue вырос:
bash Testing/f20-demo.sh topics

# 7) Завершить сессию
bash Testing/f20-demo.sh sim-off
```

## Что увидите в `watch`-логах

Цепочка от POST до sim-отчёта (≈5 сек, один matching batch cycle):

```
matching: Built auto-hedge execution intents from hedge trigger decisions
matching: Published auto-hedge execution intent (target_qty=...BTC, venue=binance)
venues:   Venues execution consumer received intent (venue=binance, BTC/USDT)
venues:   Produced sim execution report (sim.execution.venue, status=3=FILLED)
```

## Режимы маршрутизации

```bash
bash Testing/f20-demo.sh sim-on SIM_ONLY
#   intent → ТОЛЬКО sim.execution.venue. На execution.venue (live) ничего.
#   Симулятор отвечает на основе живого binance LOB. Реальные ордера НЕ
#   уходят на биржу.

bash Testing/f20-demo.sh sim-shadow
#   intent → ОБА топика: execution.venue (LIVE через cex_ws_rest_adapter)
#   И sim.execution.venue (симулятор). Для side-by-side сравнения.
#   ВНИМАНИЕ: LIVE-ветвь действительно отправит ордер на binance.

bash Testing/f20-demo.sh sim-on LIVE_ONLY
#   intent → ТОЛЬКО execution.venue. Sim-ветвь не срабатывает.
```

Хот-релоад: переключение режима / создание новой сессии применяется через
`sim.config` consumer в venues_loop, без рестарта.

## Где ещё посмотреть

**Redpanda Console** (http://localhost:8080):
- Топик `sim.execution.venue` — sim-отчёты (proto `fob.execution.v1.ExecutionReport`).
- Топик `sim.execution.annotations` — sidecar с sim-телеметрией (sim_session_id, lob_age_ms, impact_bps, latency_sample_ms).
- Топик `sim.config` — UPSERT/DELETE SimSession.
- Топик `sim.alerts` — алерты движка (stale LOB / no liquidity / timeout).
- Топики `execution.intents` / `execution.venue` — F-12 хедж-интенты и live-отчёты.

**PostgreSQL** (внутри `infra-postgres-1`):
```sql
-- активные sim-сессии
SELECT sim_session_id, name, routing_mode, scope_venues, scope_instruments,
       status, created_at, activated_at
  FROM sim_sessions
 WHERE status = 'ACTIVE';

-- состояние F-12 хедж-флоу для нашего ордера
SELECT hedge_flow_id, status, filled_qty, hedge_pnl
  FROM hedgeflows
 ORDER BY created_at DESC LIMIT 5;
```

**Venues admin REST** (`http://venues:8087/admin/v1/sim-sessions` из сети
`cex_net` — см. curl-сайдкары в скрипте). Поддерживает GET/POST/PUT/Complete.

## Конфиг F-12 (когда триггерит hedge)

Активные пороги (`infra/env/.env-example`):
- `HEDGE_TRIGGER_SYMBOLS=BTC/USDT,ETH/USDT`
- `HEDGE_TRIGGER_QTY_BTC_USDT=0.001` — fire when |position_qty| ≥ 0.001 BTC
- `HEDGE_TRIGGER_NOTIONAL_BTC_USDT=10` — fire when |position_notional| ≥ \$10 USDT
- `HEDGE_INTENT_ALLOWED_VENUES=binance,coinbase,uniswap_v3`
- `HEDGE_INTENT_STRATEGY=MARKET`, `HEDGE_INTENT_TIF=IOC`, `HEDGE_INTENT_MAX_SLIPPAGE_BPS=50`

Тестовый ордер 0.002 BTC создаёт позицию ~$140 (notional) — заведомо
выше обоих порогов.

## Известные ограничения

- `multi_real` требует исходящего сетевого доступа из контейнеров venues к
  binance/coinbase API и к Ethereum RPC для uniswap. Если хост за firewall,
  можно вернуть синтетику: установить `VENUES_ADAPTER_MODE=simulated` в
  override.
- SHADOW-режим **реально** отправляет ордер на биржу через cex_ws_rest_
  adapter. Не запускайте на mainnet без подмены creds.
- F-12 уже подписан на `execution.venue` (live). Sim-отчёты на
  `sim.execution.venue` **не** доходят до live-ledger — это правильно по
  ADR-015/ADR-016 (изоляция).

## Автоматические E2E (для CI)

- `Testing/f20_it_e2e.sh` — IT-1: SIM_ONLY full cycle (probe-впрыск intent).
- `Testing/f20_f12_e2e.sh` — IT-1 расширенный: полный путь от gateway POST.

Оба тестируют один и тот же `multi_real` венюс. Запускаются:
```bash
bash Testing/f20_it_e2e.sh
bash Testing/f20_f12_e2e.sh
```
