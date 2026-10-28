---
id: DOC-DATA-CE-AGENT-PNL-BOOK
phase: 07-data
status: proposed (2026-10-28, ADR-087)
level: sea
owner: core-team
source:
  - docs/02-system/features/F-18-ce-capital-position-hedge/feature.yaml (AC-F18v2-30)
related:
  - docs/03-architecture/adr/ADR-087-ce-agent-pnl-internal-external-total.md (решение)
  - docs/04-domain/calculations/CALC-CE-AGENT-PNL.md (формулы книги)
  - docs/07-data/ce-agent-position.md (колонки внешних сумм ce_agent_*)
  - docs/06-api/grpc/ledger-agent-pnl.md (контракт, из которого читает BFF)
---

# Data: ce_agent_pnl_book (книга внутренней биржи агента CE)

## Назначение

Хранит состояние книги сделок агента на внутренней бирже CE по средней себестоимости: остаток в штуках базового актива, среднюю цену входа и накопленный реализованный результат. Из книги и внешних сумм ([ce-agent-position](ce-agent-position.md), колонки `ext_*`) ledger считает R, U и R+U внутренней, внешних бирж и итого ([CALC-CE-AGENT-PNL](../04-domain/calculations/CALC-CE-AGENT-PNL.md)). Таблица нужна, чтобы книга переживала перезапуск ledger: без неё накопленный внутренний R обнулялся бы при каждой выкладке.

## Owner

- **Writer: ledger.** `PostgresAgentPositionRepository::SaveBooks` пишет **абсолютное состояние** изменённых книг одним пакетным запросом `INSERT … ON CONFLICT (agent_id) DO UPDATE` в конце `ApplyPositionDelta` (на такт) и после хедж-исполнения, под тем же `mu_`, что и расчёт. Идемпотентность не нужна: состояние абсолютное, повтор безвреден, сбой лечится следующим тактом. `WHERE … IS DISTINCT FROM EXCLUDED` не пишет неизменённые книги.
- **Reader: ledger** (`LoadBooks` при старте, `LoadAgentPositionsFromRepo`). BFF и фронт из таблицы не читают: числа приходят через `GetAgentPnl` (ADR-087 п. 8).
- Сброс: `ResetAgentPositions` удаляет строки книг сбрасываемых агентов (`ResetPnl`).
- История не хранится; ряд во времени остаётся в `ce_agent_pnl_history` (BFF).

## PostgreSQL (OLTP)

```sql
CREATE TABLE IF NOT EXISTS ce_agent_pnl_book (
    agent_id      TEXT PRIMARY KEY,
    agent_kind    TEXT NOT NULL CHECK (agent_kind IN ('translator', 'arbitrageur')),
    qty           NUMERIC(38, 18) NOT NULL DEFAULT 0,
    avg_price     NUMERIC(38, 18) NOT NULL DEFAULT 0 CHECK (avg_price >= 0),
    realized      NUMERIC(38, 18) NOT NULL DEFAULT 0,
    last_batch_id TEXT,
    since_ms      BIGINT NOT NULL CHECK (since_ms > 0),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT ce_agent_pnl_book_flat_chk CHECK (qty <> 0 OR avg_price = 0)
) WITH (fillfactor = 70);
```

| Поле | Смысл | Единицы |
| --- | --- | --- |
| `agent_id` | агент (имя ребра графа: `T_BTC_binance`, `A_BTC_binancekraken`); у связки обе ноги в одной книге | — |
| `agent_kind` | `translator` или `arbitrageur` | — |
| `qty` | остаток $L$: штуки базового актива, плюс — длинная позиция | штуки |
| `avg_price` | средняя цена входа $\bar p$ открытого остатка; 0 при $L=0$ | валюта котировки за штуку |
| `realized` | накопленный реализованный результат $R_{int}$ | валюта котировки |
| `last_batch_id` | последний учтённый такт клиринга: повторная доставка того же такта книгу не удваивает | — |
| `since_ms` | начало накопления (создание строки), unix мс | мс |

Инварианты строки: при $L=0$ средняя цена 0 (`CHECK`); вид агента непустой. Нарушение `CHECK` одной строки валит весь пакет, поэтому ledger нормализует строки перед записью и не пишет книги с неизвестным видом.

Индексов, кроме первичного ключа, не нужно: `LoadBooks` читает не более нескольких десятков строк при старте, `GetAgentPnl` берёт данные из памяти. `fillfactor = 70` — строки обновляются на каждом такте, место нужно для обновлений на месте (HOT).

## Нагрузка и транзакции

Запись книг — один многострочный `INSERT … ON CONFLICT` на такт (25–60 агентов, такт ≈ 1 с), а не по запросу на агента: построчная запись добавила бы к существующему коммиту на каждую ногу в `ApplyDelta` ещё столько же. Позиция и книга пишутся в разных транзакциях; сбой между ними оставляет книгу на такт позади, что само исправляется следующей дельтой агента (погрешность ограничена одним тактом).

## Миграция

Идемпотентный блок (создание таблицы, `DO`-блок с колонками `ext_*`, `rvf_base` в `ce_agent_position`) лежит в `infra/postgres/init.sql` и выполняется `ledger` сам один раз на процесс (`EnsurePnlSchema`); на живой БД рекомендуется применить его вручную **до** выкладки ledger, см. раздел «Миграция ADR-087» в [ce-agent-position](ce-agent-position.md). Если миграция не удалась (например, истёк `lock_timeout` 5 с), ledger продолжает работать по-старому: позиции читаются без новых колонок, кэш не очищается. Учёт заработка при этом приостанавливается: `LoadBooks` возвращает признак неуспеха, ledger не считает, что книг нет, не пересевает их по текущей позиции и не пишет в таблицу (иначе перезаписал бы сохранённые книги); `GetAgentPnl` отдаёт «нет данных». Повторная попытка схемы — не чаще раза в 30 с, повторная загрузка книг — по таймеру `CE_AGENT_PNL_RETRY_MS` (деф. 30 с). Окно простоя в учёт не попадает.

## Retention и сброс

Только текущее состояние, одна строка на агента. Строки удаляет сброс позиций; иных чисток нет.

## Used In

- Feature: F-18 (AC-F18v2-30, capability CAP-F18-AGENT-PNL).
- Контракт: [ledger-agent-pnl](../06-api/grpc/ledger-agent-pnl.md), REST [agent-pnl](../06-api/rest/agent-pnl.md).
