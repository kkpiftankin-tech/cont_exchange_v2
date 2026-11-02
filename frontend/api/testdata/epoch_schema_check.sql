-- Проверка схемы метки сброса (epoch_schema.js) на настоящем Postgres. Запуск: frontend/api/testdata/epoch_schema_check.sh.
-- Выполняется в одной транзакции во ВРЕМЕННОЙ схеме и откатывается; рабочие таблицы не трогает.
-- Любое нарушение — RAISE EXCEPTION, psql завершается с ошибкой (ON_ERROR_STOP).
-- Явные моменты 2026-11-02 08:00/08:05 лежат в прошлом на момент любого запуска проверки (репозиторий создан позже).

-- Минимальная копия таблиц конфигурации для досборки унаследованных эпох.
CREATE TABLE IF NOT EXISTS f05a_clearing_config (id INTEGER PRIMARY KEY, trade_stats_reset_at TIMESTAMPTZ);
CREATE TABLE IF NOT EXISTS f05a_clearing_config_history (version INTEGER, snapshot JSONB);
INSERT INTO f05a_clearing_config VALUES (1, '2026-11-02 07:02:22.169255+00') ON CONFLICT DO NOTHING;
INSERT INTO f05a_clearing_config_history VALUES
  (1, '{"trade_stats_reset_at": "2026-10-12T09:51:00.916184+00:00"}'),
  (2, '{"trade_stats_reset_at": "2026-10-12T09:51:00.916184+00:00"}'),
  (3, '{"trade_stats_reset_at": "2026-10-15T09:29:27.993677+00:00"}'),
  (4, '{"trade_stats_reset_at": null}');

DO $$
DECLARE
  n INTEGER;
  r ce_reset_epoch;
  r2 ce_reset_epoch;
  ok BOOLEAN;
BEGIN
  -- 1. Досборка: три различные метки (две из истории и текущая), пустая метка отброшена.
  n := ce_epoch_backfill_legacy();
  IF n <> 3 THEN RAISE EXCEPTION 'backfill: ожидалось 3 унаследованные эпохи, получено %', n; END IF;
  IF (SELECT count(*) FROM ce_reset_epoch WHERE kind = 'legacy' AND actor = 'migration') <> 3 THEN RAISE EXCEPTION 'backfill: kind/actor'; END IF;
  IF ce_current_epoch() <> 3 THEN RAISE EXCEPTION 'current_epoch после досборки должна быть 3, а %', ce_current_epoch(); END IF;
  IF (SELECT reset_at FROM ce_reset_epoch WHERE epoch_id = 3) <> '2026-11-02 07:02:22.169255+00'::timestamptz THEN RAISE EXCEPTION 'backfill: момент последней эпохи'; END IF;
  IF ce_epoch_backfill_legacy() <> 0 THEN RAISE EXCEPTION 'backfill не идемпотентна'; END IF;

  -- 2. Выдача новой эпохи, идемпотентность по request_id.
  r := ce_epoch_open('owner', 'absolute', 'опыт 1', 'req-1', '{"a":1}'::jsonb, '{"seed":{"BTC":"10"}}'::jsonb, 'boot-1', 16, '2026-11-02 08:00+00');
  IF r.epoch_id <> 4 OR r.prev_epoch <> 3 THEN RAISE EXCEPTION 'open: номер/prev, получено % / %', r.epoch_id, r.prev_epoch; END IF;
  r2 := ce_epoch_open('someone-else', 'bridge', 'повтор', 'req-1');
  IF r2.epoch_id <> 4 OR r2.actor <> 'owner' OR r2.kind <> 'absolute' THEN RAISE EXCEPTION 'open: повтор request_id должен вернуть ту же эпоху'; END IF;
  IF ce_current_epoch() <> 4 THEN RAISE EXCEPTION 'повтор не должен создавать эпоху'; END IF;
  r2 := ce_epoch_open('owner', 'bridge', 'опыт 2', 'req-2', '{}'::jsonb, '{}'::jsonb, NULL, NULL, '2026-11-02 08:05+00');
  IF r2.epoch_id <> 5 OR r2.prev_epoch <> 4 THEN RAISE EXCEPTION 'open: вторая эпоха'; END IF;
  IF r2.before_state <> '{}'::jsonb OR r2.initial_state <> '{}'::jsonb THEN RAISE EXCEPTION 'open: снимки по умолчанию должны быть {}'; END IF;
  -- момент по умолчанию — часы сервера БД, строго позже предыдущей метки
  r2 := ce_epoch_open('owner', 'absolute', 'опыт 3', 'req-3');
  IF r2.epoch_id <> 6 OR r2.reset_at <= '2026-11-02 08:05+00'::timestamptz OR r2.reset_at > clock_timestamp() THEN RAISE EXCEPTION 'open: момент по умолчанию'; END IF;

  -- 3. Эпоха по времени и окна.
  IF ce_epoch_at('2026-10-01 00:00+00') <> 0 THEN RAISE EXCEPTION 'epoch_at до первой метки должна быть 0'; END IF;
  IF ce_epoch_at('2026-10-12 09:51:00.916184+00') <> 1 THEN RAISE EXCEPTION 'epoch_at ровно в момент метки уже новая эпоха'; END IF;
  IF ce_epoch_at('2026-10-14 00:00+00') <> 1 THEN RAISE EXCEPTION 'epoch_at внутри эпохи 1'; END IF;
  IF ce_epoch_at(now() + interval '1 day') <> 6 THEN RAISE EXCEPTION 'epoch_at после последней метки — текущая'; END IF;
  IF (SELECT count(*) FROM ce_epoch_windows('2026-10-10 00:00+00', '2026-10-16 00:00+00')) <> 3 THEN RAISE EXCEPTION 'windows: окно 10..16 октября должно затрагивать эпохи 0, 1, 2'; END IF;
  IF (SELECT min(from_ts) FROM ce_epoch_windows('2026-10-10 00:00+00', '2026-10-16 00:00+00')) <> '2026-10-10 00:00+00'::timestamptz THEN RAISE EXCEPTION 'windows: окно обрезается по from'; END IF;
  IF (SELECT max(to_ts) FROM ce_epoch_windows('2026-10-10 00:00+00', '2026-10-16 00:00+00')) <> '2026-10-16 00:00+00'::timestamptz THEN RAISE EXCEPTION 'windows: окно обрезается по to'; END IF;
  IF (SELECT count(*) FROM ce_epoch_intervals) <> 7 THEN RAISE EXCEPTION 'intervals: виртуальная 0 и шесть меток'; END IF;

  -- 4. Защита окна для SQL-разборов.
  ok := FALSE; BEGIN PERFORM ce_guard_window('2026-10-10 00:00+00', '2026-10-16 00:00+00', NULL); EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'guard: окно через метку без причины должно отвергаться'; END IF;
  ok := FALSE; BEGIN PERFORM ce_guard_window('2026-10-10 00:00+00', '2026-10-16 00:00+00', '   '); EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'guard: пустая причина = нет причины'; END IF;
  IF ce_guard_window('2026-10-10 00:00+00', '2026-10-16 00:00+00', 'сравнение режимов') IS NOT TRUE THEN RAISE EXCEPTION 'guard: с причиной окно через метку разрешено и возвращает true'; END IF;
  IF ce_guard_window('2026-10-13 00:00+00', '2026-10-14 00:00+00', NULL) IS NOT FALSE THEN RAISE EXCEPTION 'guard: окно внутри эпохи — false без причины'; END IF;

  -- 5. Только добавление: UPDATE, DELETE, TRUNCATE запрещены.
  ok := FALSE; BEGIN UPDATE ce_reset_epoch SET note = 'x' WHERE epoch_id = 1; EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'UPDATE не запрещён'; END IF;
  ok := FALSE; BEGIN DELETE FROM ce_reset_epoch WHERE epoch_id = 6; EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'DELETE не запрещён'; END IF;
  ok := FALSE; BEGIN TRUNCATE ce_reset_epoch CASCADE; EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'TRUNCATE не запрещён'; END IF;

  -- 6. Защита от потока и подмены времени.
  ok := FALSE; BEGIN PERFORM ce_epoch_open('owner', 'bridge', 'слишком часто', 'req-fast'); EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'сброс чаще раза в минуту должен отвергаться'; END IF;
  ok := FALSE; BEGIN PERFORM ce_epoch_open('owner', 'bridge', 'в будущее', 'req-future', '{}', '{}', NULL, NULL, now() + interval '1 hour'); EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'метка из будущего должна быть отклонена'; END IF;
  ok := FALSE; BEGIN PERFORM ce_epoch_open('owner', 'bridge', 'в прошлое', 'req-past', '{}', '{}', NULL, NULL, '2026-01-01 00:00+00'); EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'метка в прошлом должна быть отклонена'; END IF;

  -- 7. Ограничения полей.
  ok := FALSE; BEGIN PERFORM ce_epoch_open('owner', 'absolute', '   ', 'req-empty', '{}', '{}', NULL, NULL, now() + interval '2 seconds'); EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'пустое название эксперимента должно быть отклонено'; END IF;
  ok := FALSE; BEGIN PERFORM ce_epoch_open('owner', 'absolute', repeat('я', 201), 'req-long', '{}', '{}', NULL, NULL, now() + interval '2 seconds'); EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'название длиннее 200 знаков должно быть отклонено'; END IF;
  ok := FALSE; BEGIN PERFORM ce_epoch_open('owner', 'absolute', E'с\tтабуляцией', 'req-ctl', '{}', '{}', NULL, NULL, now() + interval '2 seconds'); EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'управляющие символы в названии должны быть отклонены'; END IF;
  ok := FALSE; BEGIN PERFORM ce_epoch_open('owner', 'partial', 'вид', 'req-kind', '{}', '{}', NULL, NULL, now() + interval '2 seconds'); EXCEPTION WHEN OTHERS THEN ok := TRUE; END;
  IF NOT ok THEN RAISE EXCEPTION 'неизвестный вид метки должен быть отклонён'; END IF;
  IF ce_current_epoch() <> 6 THEN RAISE EXCEPTION 'отклонённые вызовы не должны создавать эпох, а текущая %', ce_current_epoch(); END IF;

  -- 8. Запуски ledger (OQ-F18v2-63): повторная вставка того же boot_id даёт одну строку, момент первого наблюдения заполнен.
  INSERT INTO ce_ledger_boot (boot_id) VALUES ('boot-a') ON CONFLICT DO NOTHING;
  INSERT INTO ce_ledger_boot (boot_id) VALUES ('boot-a') ON CONFLICT DO NOTHING;
  IF (SELECT count(*) FROM ce_ledger_boot WHERE boot_id = 'boot-a') <> 1 THEN RAISE EXCEPTION 'повторное наблюдение того же запуска должно давать одну строку'; END IF;
  IF (SELECT first_seen_at FROM ce_ledger_boot WHERE boot_id = 'boot-a') IS NULL THEN RAISE EXCEPTION 'first_seen_at не заполнен'; END IF;
  IF ce_current_epoch() <> 6 THEN RAISE EXCEPTION 'запись запуска ledger не должна менять эпоху'; END IF;
END
$$;

SELECT 'epoch_schema_check: все проверки прошли' AS result;
