'use strict';
// Схема метки сброса (эпохи) — ADR-090 п. 2, CR-F18-018, Э1. Один набор операторов для самовосстанавливающегося DDL BFF, для infra/postgres/init.sql
// (тест epoch_schema.test.js сверяет их) и для tools/reset/absolute_reset.sh. Вся логика выдачи номера эпохи — в функции ce_epoch_open
// Postgres: BFF, скрипт сброса и (с этапа Э2) ledger вызывают одну и ту же функцию, поэтому номер, момент и идемпотентность по request_id
// не расходятся между писателями.
//
// Таблица только дописывается (триггеры запрещают UPDATE, DELETE и TRUNCATE), reset_at строго растёт, номера идут подряд без дыр
// (prev_epoch = epoch_id − 1, UNIQUE(prev_epoch) исключает развилку). Эпоха 0 виртуальная («до первой метки») и в таблице не хранится.
// Все операторы — только CREATE ... IF NOT EXISTS / CREATE OR REPLACE: существующие таблицы не изменяются (урок CR-F18-015: ALTER берёт
// ACCESS EXCLUSIVE).

const STATEMENTS = [
  // 1. Таблица меток.
  `CREATE TABLE IF NOT EXISTS ce_reset_epoch (
  epoch_id       BIGINT PRIMARY KEY CHECK (epoch_id > 0),
  prev_epoch     BIGINT UNIQUE REFERENCES ce_reset_epoch (epoch_id),
  reset_at       TIMESTAMPTZ NOT NULL,
  actor          TEXT NOT NULL,
  kind           TEXT NOT NULL CHECK (kind IN ('absolute', 'bridge', 'legacy')),
  note           TEXT NOT NULL CHECK (length(btrim(note)) BETWEEN 1 AND 200 AND note !~ '[[:cntrl:]]'),
  boot_id        TEXT,
  config_version BIGINT,
  request_id     TEXT NOT NULL UNIQUE,
  before_state   JSONB NOT NULL DEFAULT '{}'::jsonb,
  initial_state  JSONB NOT NULL DEFAULT '{}'::jsonb,
  CHECK ((prev_epoch IS NULL) = (epoch_id = 1) AND (prev_epoch IS NULL OR prev_epoch = epoch_id - 1)))`,
  `CREATE UNIQUE INDEX IF NOT EXISTS ce_reset_epoch_at ON ce_reset_epoch (reset_at)`,

  // 2. Защита от изменения: только добавление, время строго растёт.
  `CREATE OR REPLACE FUNCTION ce_reset_epoch_guard() RETURNS trigger AS $$
BEGIN
  IF TG_OP = 'INSERT' THEN
    IF EXISTS (SELECT 1 FROM ce_reset_epoch WHERE reset_at >= NEW.reset_at) THEN
      RAISE EXCEPTION 'ce_reset_epoch: reset_at must strictly increase';
    END IF;
    IF NEW.reset_at > clock_timestamp() + interval '5 seconds' THEN
      RAISE EXCEPTION 'ce_reset_epoch: reset_at must not be in the future';
    END IF;
    RETURN NEW;
  END IF;
  RAISE EXCEPTION 'ce_reset_epoch is append-only (%)', TG_OP;
END;
$$ LANGUAGE plpgsql`,
  `DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_trigger WHERE tgname = 'ce_reset_epoch_guard_row' AND tgrelid = 'ce_reset_epoch'::regclass) THEN
    CREATE TRIGGER ce_reset_epoch_guard_row BEFORE INSERT OR UPDATE OR DELETE ON ce_reset_epoch
      FOR EACH ROW EXECUTE FUNCTION ce_reset_epoch_guard();
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_trigger WHERE tgname = 'ce_reset_epoch_guard_truncate' AND tgrelid = 'ce_reset_epoch'::regclass) THEN
    CREATE TRIGGER ce_reset_epoch_guard_truncate BEFORE TRUNCATE ON ce_reset_epoch
      FOR EACH STATEMENT EXECUTE FUNCTION ce_reset_epoch_guard();
  END IF;
END
$$`,

  // 3. Выдача номера эпохи: блокировка, идемпотентность по request_id, следующий номер, момент по часам сервера БД.
  `CREATE OR REPLACE FUNCTION ce_epoch_open(
  p_actor TEXT, p_kind TEXT, p_note TEXT, p_request_id TEXT,
  p_before JSONB DEFAULT '{}'::jsonb, p_initial JSONB DEFAULT '{}'::jsonb,
  p_boot_id TEXT DEFAULT NULL, p_config_version BIGINT DEFAULT NULL, p_reset_at TIMESTAMPTZ DEFAULT NULL)
RETURNS ce_reset_epoch AS $$
DECLARE
  r ce_reset_epoch;
  prev BIGINT;
  last_at TIMESTAMPTZ;
  new_at TIMESTAMPTZ;
BEGIN
  PERFORM pg_advisory_xact_lock(hashtext('ce_reset_epoch'));
  SELECT * INTO r FROM ce_reset_epoch WHERE request_id = p_request_id;
  IF FOUND THEN
    RETURN r;
  END IF;
  SELECT max(epoch_id), max(reset_at) INTO prev, last_at FROM ce_reset_epoch;
  -- Момент по часам сервера БД, но строго позже предыдущей метки (откат часов не должен блокировать сброс навсегда).
  new_at := COALESCE(p_reset_at, GREATEST(clock_timestamp(), COALESCE(last_at, '-infinity'::timestamptz) + interval '1 microsecond'));
  -- Не чаще раза в минуту (кроме унаследованных эпох): поток запросов не должен плодить необратимые эпохи.
  IF p_kind <> 'legacy' AND last_at IS NOT NULL AND new_at < last_at + interval '60 seconds' THEN
    RAISE EXCEPTION 'ce_epoch_open: too frequent (minimum interval 60 s between resets)';
  END IF;
  INSERT INTO ce_reset_epoch (epoch_id, prev_epoch, reset_at, actor, kind, note, boot_id, config_version, request_id, before_state, initial_state)
  VALUES (COALESCE(prev, 0) + 1, prev, new_at, p_actor, p_kind, p_note, p_boot_id, p_config_version,
          p_request_id, COALESCE(p_before, '{}'::jsonb), COALESCE(p_initial, '{}'::jsonb))
  RETURNING * INTO r;
  RETURN r;
END;
$$ LANGUAGE plpgsql`,

  // 4. Функции и представления для потребителей истории (SQL).
  `CREATE OR REPLACE FUNCTION ce_current_epoch() RETURNS BIGINT AS $$
  SELECT COALESCE(max(epoch_id), 0) FROM ce_reset_epoch
$$ LANGUAGE sql STABLE`,
  `CREATE OR REPLACE FUNCTION ce_epoch_at(ts TIMESTAMPTZ) RETURNS BIGINT AS $$
  SELECT COALESCE(max(epoch_id), 0) FROM ce_reset_epoch WHERE reset_at <= ts
$$ LANGUAGE sql STABLE`,
  `CREATE OR REPLACE VIEW ce_epoch_intervals AS
  SELECT 0::BIGINT AS epoch_id, '-infinity'::TIMESTAMPTZ AS from_ts,
         COALESCE((SELECT min(reset_at) FROM ce_reset_epoch), 'infinity'::TIMESTAMPTZ) AS to_ts
  UNION ALL
  SELECT epoch_id, reset_at, COALESCE(lead(reset_at) OVER (ORDER BY epoch_id), 'infinity'::TIMESTAMPTZ) FROM ce_reset_epoch`,
  `CREATE OR REPLACE FUNCTION ce_epoch_windows(p_from TIMESTAMPTZ, p_to TIMESTAMPTZ)
RETURNS TABLE (epoch_id BIGINT, from_ts TIMESTAMPTZ, to_ts TIMESTAMPTZ) AS $$
  SELECT w.epoch_id, GREATEST(w.from_ts, p_from), LEAST(w.to_ts, p_to)
    FROM ce_epoch_intervals w
   WHERE w.from_ts < p_to AND w.to_ts > p_from
   ORDER BY w.epoch_id
$$ LANGUAGE sql STABLE`,
  // Защита окна для SQL-разборов (ADR-090 п. 6): окно через метку сброса отвергается, если не названа причина. Возвращает true, если окно
  // пересекает метку (и это разрешено причиной), иначе false; вызывать как `... AND ce_guard_window(from, to, :'cross_reason') IS NOT NULL`.
  `CREATE OR REPLACE FUNCTION ce_guard_window(p_from TIMESTAMPTZ, p_to TIMESTAMPTZ, p_cross_reason TEXT DEFAULT NULL) RETURNS BOOLEAN AS $$
DECLARE
  n INTEGER;
BEGIN
  SELECT count(*) INTO n FROM ce_reset_epoch WHERE reset_at > p_from AND reset_at < p_to;
  IF n > 0 AND (p_cross_reason IS NULL OR btrim(p_cross_reason) = '') THEN
    RAISE EXCEPTION 'окно % .. % пересекает % метк(и) сброса позиций: выберите одну эпоху или разрешите явно с причиной (cross_reason)', p_from, p_to, n;
  END IF;
  RETURN n > 0;
END;
$$ LANGUAGE plpgsql STABLE`,

  // 5. Запуски ledger (OQ-F18v2-63): остатки площадок живут в памяти ledger и обнуляются при каждом перезапуске, а позиции в Postgres остаются, поэтому
  // перезапуск внутри эпохи делает эксперимент смесью. BFF записывает момент ПЕРВОГО НАБЛЮДЕНИЯ каждого boot_id (UUID без времени — точность до интервала
  // опроса BFF); инструменты разбора предупреждают, если окно пересекает перезапуск. Это граница для анализа, а не новая эпоха: ничего не сбрасывается.
  `CREATE TABLE IF NOT EXISTS ce_ledger_boot (
  boot_id       TEXT PRIMARY KEY,
  first_seen_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
)`,
];

// Досборка прошлых сбросов из истории конфигурации клиринга (колонка trade_stats_reset_at до перехода на метки была единственным следом):
// каждое различное значение — унаследованная эпоха kind = 'legacy'. Выполняется один раз, только пока таблица пуста; требует
// f05a_clearing_config и её историю (если их нет — ошибка 42P01, вызывающий код её игнорирует).
const BACKFILL_FUNCTION = `CREATE OR REPLACE FUNCTION ce_epoch_backfill_legacy() RETURNS INTEGER AS $$
DECLARE
  rec RECORD;
  n INTEGER := 0;
BEGIN
  PERFORM pg_advisory_xact_lock(hashtext('ce_reset_epoch'));
  IF EXISTS (SELECT 1 FROM ce_reset_epoch) THEN
    RETURN 0;
  END IF;
  FOR rec IN
    SELECT DISTINCT x.r
      FROM (SELECT (snapshot->>'trade_stats_reset_at')::timestamptz AS r FROM f05a_clearing_config_history
            UNION ALL
            SELECT trade_stats_reset_at FROM f05a_clearing_config) x
     WHERE x.r IS NOT NULL
     ORDER BY x.r
  LOOP
    PERFORM ce_epoch_open('migration', 'legacy', 'legacy: trade_stats_reset_at',
                          'legacy-' || to_char(rec.r AT TIME ZONE 'UTC', 'YYYYMMDD"T"HH24MISSUS'),
                          '{}'::jsonb, '{}'::jsonb, NULL, NULL, rec.r);
    n := n + 1;
  END LOOP;
  RETURN n;
END;
$$ LANGUAGE plpgsql`;

// Все операторы в порядке выполнения (досборка — последняя: ей нужны остальные функции).
const ALL_STATEMENTS = [...STATEMENTS, BACKFILL_FUNCTION];

// Создаёт схему меток. Операторы выполняются по одному; создание в одной сессии с коротким lock_timeout, чтобы не встать в очередь за
// долгой транзакцией. pool — pg.Pool. Досборка выполняется отдельным вызовом backfillLegacyEpochs: она нужна не при каждом запуске.
async function ensureEpochSchema(pool) {
  const c = await pool.connect();
  let broken = false;
  try {
    await c.query('BEGIN');
    await c.query("SET LOCAL lock_timeout = '5s'");
    for (const sql of ALL_STATEMENTS) await c.query(sql);
    await c.query('COMMIT');
  } catch (e) {
    broken = true;
    try { await c.query('ROLLBACK'); broken = false; } catch (_) { /* соединение оборвано: в пул не возвращаем */ }
    throw e;
  } finally { c.release(broken); }
}

// Однократная досборка унаследованных эпох (только пока таблица пуста). Возвращает число созданных эпох; если f05a_clearing_config или её
// истории ещё нет — 0 (метки появятся первым настоящим сбросом).
async function backfillLegacyEpochs(pool) {
  try {
    const r = await pool.query('SELECT ce_epoch_backfill_legacy() AS n');
    return Number(r.rows[0] && r.rows[0].n) || 0;
  } catch (e) {
    if (e && e.code === '42P01') return 0;
    throw e;
  }
}

// Текст всех операторов для psql (init.sql, проверка на живой БД, скрипт сброса): `node epoch_schema.js --print-sql`.
function sqlScript() {
  return ALL_STATEMENTS.map((s) => s + ';').join('\n\n') + '\n';
}

module.exports = { STATEMENTS, BACKFILL_FUNCTION, ALL_STATEMENTS, ensureEpochSchema, backfillLegacyEpochs, sqlScript };

if (require.main === module && process.argv.includes('--print-sql')) process.stdout.write(sqlScript());
