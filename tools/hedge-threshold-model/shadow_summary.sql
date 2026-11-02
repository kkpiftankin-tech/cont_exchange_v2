-- Сводка теневого режима по агентам-переводчикам (ADR-088): оценки потока, режимы и пороги модели
-- Источник — ce_agent_band_history (минутные записи BFF). Параметры psql:
--   hours         — окно в часах (по умолчанию окно усекается до начала ТЕКУЩЕЙ эпохи сброса, ADR-090);
--   est           — минимум минут оценки потока для медиан σ_z и μ;
--   epoch         — current (по умолчанию) | номер эпохи (целиком) | all;
--   cross_reason  — причина, по которой разрешено окно через метку сброса (нужна для epoch=all; пусто = пересекать нельзя);
--   warmup_min    — исключить первые N минут эпохи (прогрев оценок потока модели, ADR-090/OQ-F18v2-62); по умолчанию 60, 0 — не исключать;
--                   действует при epoch=current или номере эпохи, не при epoch=all.
--   psql -v hours=3 -v est=60 -At -F $'\t' -f tools/hedge-threshold-model/shadow_summary.sql
--   psql -v hours=72 -v est=60 -v epoch=all -v cross_reason='сравнение режимов' ... -f tools/hedge-threshold-model/shadow_summary.sql
-- Вывод — одна строка на (агент, эпоха): agent_id, минут в окне, медианы σ, ς_z, μ, b_m, b_t, z_lim, z_mkt, доли режимов,
-- максимум |позиции|, доля времени позиции выше рабочего порога тейкера, epoch_id (последняя колонка — позиции прежних колонок не менялись).
-- (Приведение номера эпохи записано через regexp_replace: Postgres вычисляет константные подвыражения заранее, и прямое ('current')::bigint падало бы
-- даже в неиспользуемой ветке CASE.)
-- Медианы по смеси эпох недопустимы: окно через метку сброса без cross_reason отвергается функцией ce_guard_window, а при разрешении
-- каждая эпоха считается отдельной строкой. Дальше — shadow_costs.js (затраты хеджа на измеренных параметрах).
\if :{?epoch}
\else
\set epoch current
\endif
\if :{?cross_reason}
\else
\set cross_reason ''
\endif
\if :{?warmup_min}
\else
\set warmup_min 60
\endif
WITH base AS (
  -- es — начало эпохи (для 'all' нет), t0 — конец окна
  SELECT CASE WHEN :'epoch' = 'all' THEN NULL
              WHEN :'epoch' = 'current' THEN (SELECT from_ts FROM ce_epoch_intervals WHERE epoch_id = ce_current_epoch())
              ELSE (SELECT from_ts FROM ce_epoch_intervals WHERE epoch_id = NULLIF(regexp_replace(:'epoch', '[^0-9]', '', 'g'), '')::bigint) END AS es,
         CASE WHEN :'epoch' ~ '^[0-9]+$' THEN (SELECT LEAST(to_ts, now()) FROM ce_epoch_intervals WHERE epoch_id = NULLIF(regexp_replace(:'epoch', '[^0-9]', '', 'g'), '')::bigint)
              ELSE now() END AS t0
), win AS (
  -- прогрев исключается, только если эпоха длиннее его: иначе окно осталось бы пустым (Python-разборы в этом случае тоже оставляют всю эпоху)
  SELECT CASE WHEN :'epoch' = 'all' THEN now() - make_interval(hours => :hours)
              WHEN :'epoch' = 'current' THEN GREATEST(now() - make_interval(hours => :hours),
                                                      CASE WHEN es + make_interval(mins => :warmup_min) < t0 THEN es + make_interval(mins => :warmup_min) ELSE es END)
              ELSE CASE WHEN es + make_interval(mins => :warmup_min) < t0 THEN es + make_interval(mins => :warmup_min) ELSE es END END AS from_ts,
         t0 AS to_ts
  FROM base
)
SELECT h.agent_id,
       count(*)                                                                  AS minutes,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY sigma_bps))::numeric, 2)  AS sigma_bps,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY sz_est) FILTER (WHERE est_minutes >= :est))::numeric, 3) AS sz_est,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY mu_est) FILTER (WHERE est_minutes >= :est))::numeric, 3) AS mu_est,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY b_m) FILTER (WHERE computed))::numeric, 2) AS b_m,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY b_t) FILTER (WHERE computed))::numeric, 2) AS b_t,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY z_lim))::numeric, 2)   AS z_lim,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY z_mkt))::numeric, 2)   AS z_mkt,
       round((avg((regime = 'noise')::int) FILTER (WHERE computed))::numeric, 2)  AS share_noise,
       round((avg((regime = 'mixed')::int) FILTER (WHERE computed))::numeric, 2)  AS share_mixed,
       round((avg((regime = 'drift')::int) FILTER (WHERE computed))::numeric, 2)  AS share_drift,
       round((avg((NOT computed)::int))::numeric, 2)                              AS share_not_computed,
       round(max(abs(position))::numeric, 2)                                      AS max_abs_pos,
       round((avg((abs(position) > z_mkt)::int))::numeric, 2)                     AS share_above_zmkt,
       ce_epoch_at(h.ts)                                                          AS epoch_id
  -- epoch-guard: window (окно win и ce_guard_window)
  FROM ce_agent_band_history h, win w
 WHERE h.ts >= w.from_ts AND h.ts < w.to_ts AND h.kind = 'translator'
   AND ce_guard_window(w.from_ts, w.to_ts, :'cross_reason') IS NOT NULL
 GROUP BY h.agent_id, ce_epoch_at(h.ts)
 ORDER BY h.agent_id, ce_epoch_at(h.ts);
