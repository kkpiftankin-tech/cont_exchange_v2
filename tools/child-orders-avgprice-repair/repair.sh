#!/bin/bash
# Восстановление child_orders.avg_price по отчётам исполнения из ClickHouse (дефект ApplyReport: цена затиралась нулём при терминальном
# отчёте без нового fill, а у заявок с несколькими fill'ами хранился VWAP только последнего тика). Запускать НА DEV-ХОСТЕ после выкладки
# исправленного venues. Режим по умолчанию — сухой прогон (только счётчики); применить: APPLY=1 bash repair.sh.
#
# VWAP заявки = Σ(прирост filled_qty × average_price отчёта) / Σ прироста по отчётам с ценой > 0 (average_price отчёта — цена прироста).
# Окно упорядочено по накопленному filled_qty (он монотонен): дубли отчётов дают прирост 0, перестановка по времени приращение не искажает.
# Точность: в ClickHouse цены и объёмы хранятся Float64 с 6 значащими цифрами (так их пишет consumer), поэтому восстановленная цена отличается
# от точной не более чем на 5e-7 относительно. Строки, где расхождение с ClickHouse меньше 1e-6, не трогаются.
# Резервная копия первоначальных значений: child_orders_avgfix_backup_20261022 (child_order_id, avg_price). Откат (перезапишет и цену строк,
# которые после ремонта обновил исправленный venues, поэтому откатывать только целиком и сразу):
#   UPDATE child_orders c SET avg_price = b.avg_price FROM child_orders_avgfix_backup_20261022 b WHERE c.child_order_id = b.child_order_id;
set -euo pipefail
SINCE='2026-10-22 07:50:00'          # UTC; резидентные заявки включены в 08:04, берём с запасом
SINCE_MS=$(date -u -d "$SINCE" +%s)000
P="docker exec -i infra-postgres-1 psql -U cex -d cex -v ON_ERROR_STOP=1"

docker exec infra-clickhouse-1 clickhouse-client -q "
SELECT client_order_id, sum(d * average_price) / sum(d) AS vwap, max(filled_qty) AS filled
FROM (
  SELECT client_order_id, average_price, filled_qty,
         filled_qty - lagInFrame(filled_qty, 1, 0.) OVER (PARTITION BY client_order_id ORDER BY filled_qty, event_time_ms, ingested_at ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS d
  FROM default.execution_venue
  WHERE event_time_ms >= $SINCE_MS AND client_order_id LIKE 'ce|band|%' AND filled_qty > 0
)
WHERE d > 0 AND average_price > 0
GROUP BY client_order_id
FORMAT TSV" > /tmp/ch_vwap.tsv
echo "ClickHouse: $(wc -l < /tmp/ch_vwap.tsv) заявок с fill"

$P -q -c "DROP TABLE IF EXISTS tmp_ch_vwap; CREATE TABLE tmp_ch_vwap (client_order_id text PRIMARY KEY, vwap numeric, filled numeric)"
$P -q -c "COPY tmp_ch_vwap FROM STDIN" < /tmp/ch_vwap.tsv

# Кандидаты: есть fill, строка не менялась последние 5 минут (отчёты уже в ClickHouse), расхождение с VWAP больше 1e-6.
COND="c.created_at >= '$SINCE+00' AND c.filled_qty > 0 AND c.updated_at < now() - interval '5 minutes'
      AND abs(COALESCE(c.avg_price, 0) - t.vwap) / t.vwap > 1e-6"
$P -c "SELECT count(*) AS кандидатов, count(*) FILTER (WHERE COALESCE(c.avg_price,0) = 0) AS с_нулевой_ценой FROM child_orders c JOIN tmp_ch_vwap t USING (client_order_id) WHERE $COND"

if [ "${APPLY:-0}" = "1" ]; then
  $P <<SQL
BEGIN;
CREATE TABLE IF NOT EXISTS child_orders_avgfix_backup_20261022 (
  child_order_id text PRIMARY KEY, client_order_id text, avg_price numeric, filled_qty numeric, backed_up_at timestamptz DEFAULT now());
-- ON CONFLICT DO NOTHING: при повторном запуске сохраняется ПЕРВОНАЧАЛЬНОЕ значение, а не результат прошлого ремонта.
INSERT INTO child_orders_avgfix_backup_20261022 (child_order_id, client_order_id, avg_price, filled_qty)
  SELECT c.child_order_id, c.client_order_id, c.avg_price, c.filled_qty
    FROM child_orders c JOIN tmp_ch_vwap t USING (client_order_id) WHERE $COND
  ON CONFLICT (child_order_id) DO NOTHING;
UPDATE child_orders c SET avg_price = t.vwap
  FROM tmp_ch_vwap t
 WHERE c.client_order_id = t.client_order_id AND $COND;
COMMIT;
SELECT 'после: заявок с fill и нулевой ценой' AS what, count(*) FROM child_orders
 WHERE created_at >= '$SINCE+00' AND filled_qty > 0 AND client_order_id LIKE 'ce|band|%' AND COALESCE(avg_price,0) = 0;
SQL
else
  echo "Сухой прогон: изменений нет. Применить: APPLY=1 bash $0"
fi
$P -q -c "DROP TABLE IF EXISTS tmp_ch_vwap"
