---
id: DOC-DATA-CE-AGENT-POSITION
phase: 07-data
status: schema-pending-impl
level: sea
owner: core-team
source:
  - incoming-docs/2026-09-15-CE_algorithm_v2.md (A2, A6, A7, A8)
related:
  - docs/03-architecture/adr/ADR-061-ce-v2-agent-position-model.md (TODO — supersedes ADR-057/058)
  - docs/07-data/ce-agent-nodes.md (v1 форма — Conflict Notes ниже)
  - docs/06-api/grpc/ledger-agent-positions.md
  - docs/06-api/messaging/ce-position-delta.md
  - contracts/proto/fob/ledger/v1/ledger.proto
  - contracts/proto/fob/treasury/v1/treasury.proto
---

# Data: CE Agent Position (позиция виртуального контрагента, v2)

> **Status:** ⧗ DDL — при реализации (F-18 v2, стадия кода T-F18-007..011). Этот файл —
> МАРКДАУН-описание схемы; собственно DDL (`init.sql`) и proto не редактируются на этом
> шаге (docs-first, CLAUDE.md §11.2).
>
> Деньги/объёмы — `NUMERIC(38,18)` (PG), mirror `fob.common.v1.Decimal` (CLAUDE.md §9).
> Диагностика такта (`σ, α, π, невязка`) — `double`; персист пересекает границу
> `double → Decimal` ровно в момент записи в эту таблицу (A6/A7).
>
> Таблица за флагом CE-агентов v2; при выключении не читается (обратимость ADR-061).

## Назначение

`ce_agent_position` — **источник истины для позиции агента `c_j`** между тактами клиринга.
В новой постановке (v2, `incoming-docs/2026-09-15-CE_algorithm_v2.md`) агенты — это только
**переводчики** (`Q_*`, маркет-мейкеры на одной площадке) и **арбитражёры** (`T_*`, перевоз
одного актива между площадками). У каждого агента:

- позиция `c_j` **стартует с нуля**;
- позиция **знаковая** — отрицательная легитимна (A6: базис ядра `W·c=0` имеет противоположную
  компоненту; требование `c ≥ 0` дало бы только `c = 0`, т.е. отсутствие оборота);
- позиция — это **и есть** накопленное неисполненное обещание («committed» из v1 больше не
  отдельное поле, а сама позиция);
- пока `|c_j| ≤ q_j` (полоса), наружу не эмитится ни одной заявки (A7);
- персист **обязателен**: от `c_j` зависят и якорь A2 (`σ*_j = m_j + ρ_j · c_j`), и эмиссия
  A7 (`|c_j| > q_j`). Потеря `c_j` при рестарте = агент «забывает» накопленное обещание и
  нарушает сетевой инвариант `W·c = 0`, если другие агенты пережили рестарт иначе.

Отличие от клиентского баланса: `accounts.free_balance` неотрицателен (CHECK); позиция агента
знаковая и стартует с нуля — **несовместимые инварианты**, поэтому агент живёт в отдельной
таблице (см. раздел «party_type» ниже), а не в `accounts`.

## Owner

- Writer: **ledger** — консюмер топика `ce.position.delta` (сообщение `AgentDelta`,
  см. [ce-position-delta.md](../06-api/messaging/ce-position-delta.md)); читает по RPC
  `GetAgentPositions` ([ledger-agent-positions.md](../06-api/grpc/ledger-agent-positions.md)).
- Readers: **matching** (A2 читает `c_j` ДО накопления следующего такта), **risk**
  (мониторинг полосы `|c_j| ≤ q_j`), **frontend-api / BFF**, **observability**.

## PostgreSQL (OLTP)

### `ce_agent_position` (new — ADR-061)

Состояние позиции агента между тактами.

| Column | Type | Notes |
| --- | --- | --- |
| `agent_id` | `TEXT` | `'Q_BIN'`, `'Q_KRK'`, `'T_BTC'`, `'T_USD'`, … — ребро графа (тип+узлы) |
| `asset` | `TEXT` | `'BTC'`, `'USD'`, … |
| `venue` | `TEXT` | для переводчика — площадка узла; для арбитражёра — конвенция «dst-узел канонического направления» при `V=2` (см. Open Question) |
| `agent_kind` | `TEXT` | `CHECK (agent_kind IN ('translator','arbitrageur'))` |
| `position` | `NUMERIC(38,18)` | `c_j`, **знаковая**, `DEFAULT 0`, **без** CHECK неотрицательности (A6: отрицательная позиция обязательна) |
| `last_batch_id` | `TEXT` | последний такт, менявший позицию → `clearing_trace.batch_id` (CH); ключ идемпотентности применения дельты |
| `updated_at` | `TIMESTAMPTZ` | `DEFAULT now()` |

PK `(agent_id, asset, venue)` — трёхсоставной; прецедент — `sim_positions`
(ADR-016, `infra/postgres/init.sql`). Вспомогательный индекс
`(agent_kind, updated_at DESC)` — для запросов «все переводчики вне полосы» и
staleness-мониторинга.

### Upsert-семантика с guard по `last_batch_id` (идемпотентность per batch)

За один такт на агента приходит ОДНА дельта = `f_j` (A6, каждый такт) за вычетом
эмитированного (A7, только при выходе за `±q`). `ledger` применяет её одним upsert. Kafka
`ce.position.delta` — at-least-once, поэтому дельта может прийти повторно; guard по
`last_batch_id` защищает от двойного применения (CLAUDE.md §17 — «применять fill дважды»
запрещено):

```sql
INSERT INTO ce_agent_position
  (agent_id, asset, venue, agent_kind, position, last_batch_id, updated_at)
VALUES ($1, $2, $3, $4, $5, $6, now())
ON CONFLICT (agent_id, asset, venue) DO UPDATE
  SET position      = ce_agent_position.position + EXCLUDED.position,  -- c ← c + Δc (A6)
      last_batch_id = EXCLUDED.last_batch_id,
      updated_at    = now()
  WHERE ce_agent_position.last_batch_id IS DISTINCT FROM EXCLUDED.last_batch_id;
```

Допущение: один `AgentDelta` на пару `(agent_id, batch_id)`. Повторная доставка того же
`batch_id` не меняет строку (`WHERE ... IS DISTINCT FROM` → no-op).

### Денежная граница

`σ, α, c, π` в клиринговом ядре — `double` (диагностика). `c_j` пересекает границу
`double → Decimal` **ровно в момент записи** в эту таблицу (A6/A7) — единственная точка, где
позиция становится persist-истиной. Маппинг обязан идти через `fob.common.v1.Decimal` с явным
`scale`, без прямого `double → NUMERIC` каста без округления (CLAUDE.md §9).

## `ce_transfer` (state machine перевоза)

Физический перевоз актива между площадками, эмитируемый A7 для арбитражёра. Оперативное
in-flight состояние (класс `hedgeflows`/`child_orders`) — источник истины в PG. Полная DDL —
стадия кода F-18 v2 Э5; здесь фиксируется **машина состояний**, от которой зависит
идемпотентность возврата в позицию.

| Column | Type | Notes |
| --- | --- | --- |
| `transfer_id` | `UUID PK` | `gen_random_uuid()`, ключ идемпотентности |
| `agent_id` | `TEXT` | эмитент (`'T_BTC'`) |
| `asset`, `src_venue`, `dst_venue` | `TEXT` | `CHECK (src_venue <> dst_venue)` |
| `qty` | `NUMERIC(38,18)` | `CHECK (qty > 0)` |
| `tariff_bps` | `NUMERIC(38,18)` | тариф маршрута |
| `eta_ms` | `INTEGER` | задержка на момент создания (аудит; «Незакрытое» п.2 алгоритма — сейчас ~3 такта) |
| `eta_at` | `TIMESTAMPTZ` | `created_at + eta_ms`; абсолютный дедлайн sweep-запроса A8.1 |
| `batch_id` | `TEXT` | такт-эмитент (A7) → `clearing_trace.batch_id` |
| `status` | `TEXT` | `CHECK (status IN ('in_transit','delivered','timed_out','returned'))` |
| `created_at`, `delivered_at` | `TIMESTAMPTZ` | |

State machine:

```text
in_transit ──(A8.1: eta_at<=now, приход применён к остатку узла назначения)──▶ delivered
in_transit ──(A8.3: дедлайн наступил, остаток не исполнен)──▶ timed_out ──(возврат применён)──▶ returned
```

Двухшаговый терминальный переход `timed_out → returned` разделяет «дедлайн наступил» и
«ledger уже применил возврат остатка в позицию агента по ПЛАНОВОЙ цене» (A8.3). Без этого
разделения повторный sweep вернул бы остаток дважды — тот же idempotency-паттерн, что и guard
по `last_batch_id` в `ce_agent_position`. Sweep-запрос A8.1 опирается на частичный индекс
`(status, eta_at) WHERE status = 'in_transit'`.

Writer: `venues` (симулятор перевоза, F-20). `matching` только эмитирует запрос (A7).
Readers: `venues` (свой sweep), `ledger`/`matching` (A8.1 приход, A8.3 возврат), frontend-api,
observability.

## `party_type`: CLIENT / HOUSE / AGENT

Три типа участника имеют **несовместимые инварианты**, поэтому НЕ сводятся в одну таблицу:

| | CLIENT | HOUSE | AGENT |
| --- | --- | --- | --- |
| хранилище | `accounts(user_id, asset)` | `accounts(user_id='__ce_house__', asset)` — та же схема | **новая** `ce_agent_position(agent_id, asset, venue)` |
| знак | `free_balance >= 0` (CHECK) | по факту тоже `>= 0` | **знаковая**, без CHECK |
| резерв | есть (`reserved_balance`) | колонка есть, содержательно не используется | **нет концепта резерва** |
| ключ | `(user_id, asset)` | `(user_id, asset)` (magic string) | `(agent_id, asset, venue)` |

Впихнуть AGENT в `accounts` нельзя: пришлось бы снять `CHECK (free_balance >= 0)` для всей
таблицы (тихо ослабляет инвариант неотрицательности клиентских балансов — недопустимо,
CLAUDE.md §8.2) или добавлять nullable-колонки под чужой ключ.

- **CLIENT / HOUSE** остаются в `accounts` — различаются через `party_type` как **generated
  column** (не меняет инварианты, не требует переписи CHECK, безопасно для `ALTER` на живой
  таблице, Postgres 12+):

  ```sql
  ALTER TABLE accounts
    ADD COLUMN IF NOT EXISTS party_type TEXT
      GENERATED ALWAYS AS (CASE WHEN user_id = '__ce_house__' THEN 'HOUSE' ELSE 'CLIENT' END) STORED;
  ```

  Заменяет разбросанные по SQL `WHERE user_id <> '__ce_house__'` на `WHERE party_type = 'CLIENT'`
  без смены семантики хранения.

- **AGENT** — отдельная таблица `ce_agent_position`. На уровне API/proto три типа унифицируются
  как дискриминатор `enum PartyType { CLIENT, HOUSE, AGENT }` для трёх разных RPC
  (`GetAccountBalance` → CLIENT, `GetExchangeBalances` → HOUSE, `GetAgentPositions` → AGENT),
  при физически разных хранилищах.

## Миграция

Механизма миграций у `ledger` **нет**: `infra/postgres/init.sql` монтируется в
`/docker-entrypoint-initdb.d/` и выполняется Postgres **только на пустом `PGDATA`**. Любые
`ALTER TABLE ... ADD COLUMN` / новые `CREATE TABLE`, дописанные после факта, на уже поднятых
dev/staging контейнерах автоматически **не применяются** (прецедент —
`f05a_clearing_config.ce_taker_fee_bps`, гонялся руками).

Для F-18 v2 (эта таблица) миграция **ручная**:

1. `ce_agent_position` и `ce_transfer` приземляются как обычные `CREATE TABLE IF NOT EXISTS`
   в существующий единый `init.sql` (тот же stopgap-паттерн) — стадия кода T-F18-007..011.
2. `party_type` — `ADD COLUMN IF NOT EXISTS ... GENERATED` на `accounts`.
3. На живых dev/staging БД те же выражения выполняются **вручную** (`psql -f`), с явной
   пометкой в PR-описании и записью в `docs/07-data/migrations.md` (файла ещё нет — создаётся
   при реализации).

**Предложение (не в этом PR): Flyway-lite runner.** Разбить `init.sql` на пронумерованные
идемпотентные `infra/postgres/migrations/NNN_*.sql`, добавить `apply-migrations.sh` +
bookkeeping-таблицу `schema_migrations(filename PK, applied_at)`, CI-гейт «прогнать дважды,
второй прогон no-op». Это архитектурное решение по инфраструктуре персистентности → требует
**отдельного ADR** и отдельного PR; F-18 v2 им не блокируется.

## Used In

- Feature: F-18 v2 (CE-агенты v2, виртуальные контрагенты)
- Источник алгоритма: [`incoming-docs/2026-09-15-CE_algorithm_v2.md`](../../incoming-docs/2026-09-15-CE_algorithm_v2.md)
- Контракт (RPC): [ledger-agent-positions.md](../06-api/grpc/ledger-agent-positions.md)
- Контракт (Kafka): [ce-position-delta.md](../06-api/messaging/ce-position-delta.md)

## Related ADR

- ADR-061 (TODO) — модель позиции агента v2, supersedes ADR-057 (трёхуровневая позиция) и
  ADR-058 (committed/in-flight state).

## Open Questions (владельцу, не решаются здесь)

1. `venue` в PK для арбитражёра неоднозначен: `c_j` арбитражёра — единственное число на агента
   (не per-node), но A3 даёт ему грань между ДВУМЯ узлами (`BTC@Binance −1`, `BTC@Kraken +1`).
   Конвенция для `V=2`: `venue` = площадка узла с `+1` в столбце `W` («пункт назначения»),
   `agent_id` уже уникально идентифицирует маршрут. При `V>2` потребуется расширить ключ до
   `(agent_id, asset, src_venue, dst_venue)` или завести `ce_agent_route` — это «Незакрытое»
   п.3 алгоритма применительно к ключу таблицы. Для текущего кода (`V=2`) не блокирует.
2. Нумерация `ADR-061` — рабочая ссылка; финальный номер ADR подтверждает владелец.
