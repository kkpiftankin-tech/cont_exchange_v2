-- Проверка SQL обновления child_orders.avg_price в PostgresChildOrderRepository::ApplyReport (накопленный VWAP, без затирания нулём).
-- Запуск (боевые данные не затрагиваются: временная таблица и ROLLBACK; при неверном значении скрипт падает с исключением FAIL):
--   docker exec -i infra-postgres-1 psql -U cex -d cex -v ON_ERROR_STOP=1 < cpp/venues/tests/sql/child_orders_apply_report_avg_price.sql
-- Формулу держать в синхроне с postgres_child_order_repository.cpp. PREPARE без списка типов повторяет вывод типов параметров при exec_params
-- (pqxx передаёт текстовые значения без типов): $3 приводится к NUMERIC, $4 остаётся текстом внутри NULLIF.
BEGIN;
CREATE TEMP TABLE t (hedge_flow_id text, client_order_id text, filled_qty numeric NOT NULL DEFAULT 0, avg_price numeric,
                     venue_order_id text, status text, error_code text, error_message text, updated_at timestamptz);
CREATE FUNCTION pg_temp.chk(step text, got numeric, want numeric) RETURNS void AS $$
BEGIN
  IF got IS DISTINCT FROM want THEN
    RAISE EXCEPTION 'FAIL %: получено %, ожидалось %', step, got, want;
  END IF;
END $$ LANGUAGE plpgsql;

INSERT INTO t(hedge_flow_id, client_order_id, status) VALUES
  ('h','c1','PENDING'),('h','c2','PENDING'),('h','c3','PENDING'),('h','c4','PENDING'),('h','c5','PENDING'),('h','c6','PENDING');
UPDATE t SET filled_qty = 1.0, avg_price = 0   WHERE client_order_id = 'c3';   -- строка, испорченная старым кодом
UPDATE t SET filled_qty = 1.0, avg_price = NULL WHERE client_order_id = 'c5';  -- fill есть, цены нет

PREPARE u AS
UPDATE t
   SET avg_price     = COALESCE(CASE
         WHEN COALESCE(NULLIF($4,'')::NUMERIC, 0) <= 0 OR $3::NUMERIC <= filled_qty THEN avg_price
         WHEN filled_qty <= 0 OR avg_price IS NULL OR avg_price = 0 THEN NULLIF($4,'')::NUMERIC
         ELSE (avg_price * filled_qty + NULLIF($4,'')::NUMERIC * ($3::NUMERIC - filled_qty)) / $3::NUMERIC
       END, 0),
       filled_qty    = GREATEST(filled_qty, $3::NUMERIC),
       venue_order_id = COALESCE(NULLIF($5,''), venue_order_id),
       status        = $6,
       error_code    = COALESCE(NULLIF($7,''), error_code),
       error_message = COALESCE(NULLIF($8,''), error_message),
       updated_at    = now()
 WHERE hedge_flow_id  = $1 AND client_order_id = $2;

-- c1: fill 0.5 по 100; затем рост до 1.0 по 110 (прирост 0.5) ⇒ VWAP 105; терминальный отчёт без цены; повтор; нулевая цена; перестановка
EXECUTE u('h','c1','0.5','100','','PARTIALLY_FILLED','','');
SELECT pg_temp.chk('c1 первый fill: объём', filled_qty, 0.5), pg_temp.chk('c1 первый fill: цена', avg_price, 100) FROM t WHERE client_order_id='c1';
EXECUTE u('h','c1','1.0','110','','PARTIALLY_FILLED','','');
SELECT pg_temp.chk('c1 VWAP двух fill', avg_price, 105), pg_temp.chk('c1 объём', filled_qty, 1.0) FROM t WHERE client_order_id='c1';
EXECUTE u('h','c1','1.0','','','CANCELLED','','');
SELECT pg_temp.chk('c1 терминальный без цены: цена не затёрта', avg_price, 105) FROM t WHERE client_order_id='c1';
EXECUTE u('h','c1','1.0','110','','CANCELLED','','');
SELECT pg_temp.chk('c1 повторная доставка: цена прежняя', avg_price, 105) FROM t WHERE client_order_id='c1';
EXECUTE u('h','c1','1.5','0.000','','CANCELLED','','');
SELECT pg_temp.chk('c1 нулевая цена при росте объёма: цена прежняя', avg_price, 105), pg_temp.chk('c1 объём вырос без цены', filled_qty, 1.5) FROM t WHERE client_order_id='c1';
EXECUTE u('h','c1','0.7','90','','CANCELLED','','');
SELECT pg_temp.chk('c1 запоздавший отчёт с меньшим объёмом: объём не падает', filled_qty, 1.5), pg_temp.chk('c1 запоздавший отчёт: цена прежняя', avg_price, 105) FROM t WHERE client_order_id='c1';

-- c2: отклонённая заявка без fill: 0 и 0 (как до исправления)
EXECUTE u('h','c2','0','','','REJECTED','E','x');
SELECT pg_temp.chk('c2 объём', filled_qty, 0), pg_temp.chk('c2 цена 0, не NULL', avg_price, 0) FROM t WHERE client_order_id='c2';

-- c3: строка с затёртой ценой (объём 1.0, цена 0), приходит рост до 1.5 по 120 ⇒ цена отчёта
EXECUTE u('h','c3','1.5','120','','PARTIALLY_FILLED','','');
SELECT pg_temp.chk('c3 испорченная строка: цена отчёта', avg_price, 120), pg_temp.chk('c3 объём', filled_qty, 1.5) FROM t WHERE client_order_id='c3';

-- c4: заявка исполнена целиком одним отчётом (IOC, маркет): поведение прежнее
EXECUTE u('h','c4','2','99.5','v1','FILLED','','');
SELECT pg_temp.chk('c4 цена', avg_price, 99.5), pg_temp.chk('c4 объём', filled_qty, 2) FROM t WHERE client_order_id='c4';
SELECT pg_temp.chk('c4 venue_order_id записан', CASE WHEN venue_order_id = 'v1' THEN 1 ELSE 0 END, 1) FROM t WHERE client_order_id='c4';

-- c5: fill есть, цены нет (NULL), приходит рост по 130
EXECUTE u('h','c5','2.0','130','','PARTIALLY_FILLED','','');
SELECT pg_temp.chk('c5 NULL-цена: цена отчёта', avg_price, 130) FROM t WHERE client_order_id='c5';

-- c6: объём растёт без цены (цены нет в отчёте), потом приходит цена
EXECUTE u('h','c6','0.4','','','PARTIALLY_FILLED','','');
SELECT pg_temp.chk('c6 объём без цены: цена 0', avg_price, 0), pg_temp.chk('c6 объём', filled_qty, 0.4) FROM t WHERE client_order_id='c6';
EXECUTE u('h','c6','1.0','50','','PARTIALLY_FILLED','','');
SELECT pg_temp.chk('c6 цена пришла позже: цена отчёта (прежний fill без цены неизвестен)', avg_price, 50) FROM t WHERE client_order_id='c6';

SELECT 'ВСЕ ПРОВЕРКИ ПРОШЛИ' AS итог;
ROLLBACK;
