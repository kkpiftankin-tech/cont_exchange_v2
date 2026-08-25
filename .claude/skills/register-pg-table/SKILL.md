---
description: Register a new PostgreSQL table in cont_exchange_v2.0 — add `CREATE TABLE IF NOT EXISTS` to `infra/postgres/init.sql`, create documentation file in `docs/07-data/<table>.md`, link it from related feature.yaml `postgresTables` block. Use when the user asks to add / create / register a new PostgreSQL table or schema.
---

# Skill: register-pg-table

## Purpose

Полное добавление новой OLTP-таблицы в проект:
1. DDL в `infra/postgres/init.sql` (idempotent через `IF NOT EXISTS`).
2. Документация в `docs/07-data/<table>.md`.
3. Обновление `postgresTables.reads/writes` в feature.yaml тех F-XX которые работают с таблицей.

Покрывает паттерн PR-F02-001 (`flow_orders` writer для order_flow) — каждое поле описано, индексы под known query patterns, money — `NUMERIC`, money invariants — CHECK constraints.

## When to use

- "Добавь таблицу X"
- "Создай PG-таблицу для F-XX"
- "Зарегистрируй <table_name>"
- "Нужна новая OLTP-таблица для ..."

## Required inputs

- **Table name** (snake_case, множественное число: `flow_orders`, `child_orders`, `ledger_entries`)
- **Owner-сервис** (один сервис — primary writer)
- **Writers** (включая owner, обычно 1-2 сервиса)
- **Readers** (сервисы которые SELECT)
- **Columns** — список с типами PostgreSQL
- **Primary key** — column(s)
- **Indexes** — column(s) под known query patterns
- **Constraints** — CHECK, FK, UNIQUE
- **Linked features** — список F-XX

## Step-by-step

### 1. Прочитать существующее

```bash
cat infra/postgres/init.sql | head -100
ls docs/07-data/
```

Убедиться что таблица с таким именем не существует.

### 2. Добавить DDL в `infra/postgres/init.sql`

Найти **раздел** соответствующий назначению таблицы (например, `-- Trading state`, `-- Execution`, `-- Ledger`, `-- Sim sessions`). Добавить в конце раздела:

```sql
-- ---------------------------------------------------------------------------
-- <table_name>: <одна строка о назначении>
--
-- <2-3 строки о semantics, кто writer, какие query patterns>
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS <table_name> (
  <column_1>      <type>          NOT NULL,
  <column_2>      <type>          NOT NULL DEFAULT <default>,
  ...
  created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
  updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),

  PRIMARY KEY (<columns>),

  CONSTRAINT <table>_<rule>      CHECK (<expression>),
  CONSTRAINT <table>_<fk_name>   FOREIGN KEY (<col>) REFERENCES <ref_table>(<ref_col>) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_<table>_<columns>
  ON <table_name> (<columns>)
  [WHERE <condition>];  -- partial index если применимо
```

### Правила для DDL

- **Money** — `NUMERIC(38, X)` где X = scale. Помнить про PR-F02-002: scale=18 переполняет int64 в C++. Для нового кода — `NUMERIC(38, 8)` если возможно.
- **UUID PK** — `UUID PRIMARY KEY DEFAULT gen_random_uuid()` (требует `pgcrypto` extension; уже включён в init.sql).
- **Status enum как TEXT** — не Postgres ENUM (изменения дороже).
- **CHECK constraints** — все доменные инварианты (`q_max > 0`, `p_low ≤ p_high`, `filled_cum ≤ q_max`).
- **FK с ON DELETE** — явный, чтобы каскадные удаления были видны в DDL.
- **TIMESTAMPTZ** — для всех временных полей.
- **Indexes** — на (a) FK поля, (b) часто фильтруемые columns, (c) partial для статусов.

### 3. Создать `docs/07-data/<table>.md`

Шаблон:

```markdown
# Таблица: <table_name>

| Свойство | Значение |
|---|---|
| База | PostgreSQL |
| Owner-сервис | <service> |
| Writers | <service1>, <service2> |
| Readers | <service1>, <service2> |
| Создаётся | `infra/postgres/init.sql:<line>` |

## Назначение

<1-2 параграфа: какие данные хранит, зачем>

## DDL

```sql
CREATE TABLE IF NOT EXISTS <table_name> (
  ...
);
CREATE INDEX ...
```

## Колонки

| Колонка | Тип | NULL | Default | Назначение |
|---|---|---|---|---|
| `<col>` | <type> | no | <default> | <описание> |

## Инварианты

- INV-1: `<rule>` — обеспечивается CHECK `<constraint_name>`
- INV-2: ...

## Когда заполняется

| Событие | Writer | Operation |
|---|---|---|
| <event> | <service> | INSERT |
| <event> | <service> | UPDATE WHERE ... |

## Когда читается

| Query | Reader | Index used |
|---|---|---|
| <pattern> | <service> | `idx_<name>` |

## Retention

<rule>: <e.g. "Never deleted in MVP" / "Move to OLAP after 30 days" / "Hard delete after compliance window">

## Связанные feature.yaml

- [F-XX](../02-system/features/F-XX-*/feature.yaml)

## Известные ограничения

<если применимо>
```

### 4. Обновить feature.yaml связанных F-XX

```yaml
postgresTables:
  writes:
    - <table_name>      # если фича вставляет/обновляет
  reads:
    - <table_name>      # если фича только читает
```

### 5. Quality gate

```bash
# Локальная валидация DDL syntax
docker run --rm -v $PWD/infra/postgres:/sql postgres:16-alpine \
  psql -h /var/lib/empty -f /dev/null  # placeholder — реально проще на dev-хосте

# Применить на dev-хосте
ssh nik@ubuntu-dev "cd /home/nik/cont_exchange_v2/infra && \
  rsync -a postgres/init.sql nik@ubuntu-dev:/home/nik/cont_exchange_v2/infra/postgres/init.sql"

# Применить миграцию вручную (init.sql применяется только при создании volume!)
ssh nik@ubuntu-dev "docker exec -i infra-postgres-1 psql -U cex -d cex" < infra/postgres/init.sql
```

⚠️ `init.sql` применяется автоматически **только при создании volume**. Если volume уже создан — DDL надо применить вручную через `psql`. Подсказать это пользователю.

## Rules

- **Не** удалять / переименовывать существующие колонки (breaking change → нужна миграция + ADR).
- **Не** менять type существующих колонок без миграции.
- **Не** использовать `double precision` для money. Только `NUMERIC`.
- Новая таблица должна иметь хотя бы owner-сервис + 1 writer + ≥0 readers документированных.
- Документация добавляется **до** написания репозитория в `cpp/<service>/src/infra/postgres/`.
- Если таблица содержит PII (email, full name) — `docs/07-data/<table>.md` должен явно это упомянуть + retention policy.

## Output

После выполнения вернуть:

1. **Изменённые файлы**:
   - `infra/postgres/init.sql` (новый CREATE TABLE)
   - `docs/07-data/<table>.md` (новый файл)
   - `docs/02-system/features/F-XX-*/feature.yaml` (обновлены postgresTables)

2. **Команда применения на dev** (volume уже существует):
   ```bash
   ssh nik@ubuntu-dev "docker exec -i infra-postgres-1 psql -U cex -d cex" < infra/postgres/init.sql
   ```

3. **Проверка**:
   ```bash
   ssh nik@ubuntu-dev "docker exec infra-postgres-1 psql -U cex -d cex -c '\\d <table_name>'"
   ```

4. **Рекомендация следующего шага**:
   - Создать repository interface в `cpp/<service>/src/domain/`
   - Реализация — задача для cpp-service-architect → implementation-planner → code-implementer
