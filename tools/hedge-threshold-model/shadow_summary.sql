-- Сводка теневого режима по агентам-переводчикам (ADR-088): оценки потока, режимы и пороги модели против рабочих.
-- Источник — ce_agent_band_history (минутные записи BFF). Параметры: окно в часах (:hours) и минимум минут оценки (:est).
--   psql -v hours=3 -v est=60 -At -F $'\t' -f tools/hedge-threshold-model/shadow_summary.sql
-- Вывод — одна строка на агента: agent_id, минут в окне, медианы σ, ς_z, μ, b_m, b_t, z_lim, z_mkt, доли режимов, доли «не посчитано»,
-- максимум |позиции|, доля времени позиции выше рабочего порога тейкера. Дальше — shadow_costs.js (затраты модели против правила).
SELECT agent_id,
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
       round((avg((abs(position) > z_mkt)::int))::numeric, 2)                     AS share_above_zmkt
  FROM ce_agent_band_history
 WHERE ts > now() - make_interval(hours => :hours) AND kind = 'translator'
 GROUP BY agent_id
 ORDER BY agent_id;
