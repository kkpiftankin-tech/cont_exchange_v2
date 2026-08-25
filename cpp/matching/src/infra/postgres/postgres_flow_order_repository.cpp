// ============================================================================
// postgres_flow_order_repository.cpp — PG reader/writer для matching-сервиса.
//
// Назначение:
//   На каждом батч-тике matching должен (а) загрузить активные FlowOrder из
//   flow_orders + flow_order_legs, чтобы передать их solver'у, и (б) после
//   solve и формирования fills записать накопленный filled_cum обратно в PG,
//   потенциально переведя заявку в статус filled / partially_filled.
//
//   Read-path:  LoadActiveFlowOrders(as_of_time, instrument?, limit?)
//   Write-path: UpdateFilledVolumes(deltas, batch_time)
//
// Ключевые исторические фиксы (см. file header'ы decimal_conversion.cpp и
// CLAUDE.md §15):
//   * PR-F02-002 — fix NUMERIC overflow при чтении p_low/p_high/q_max
//     (NUMERIC(38,18) → trailing-zero strip в ParsePgNumeric).
//   * PR-F02-005 — fix solver NaN для SELL заявок из PG: matching domain
//     ожидает signed-by-side p_low/p_high (SELL: domain.p_low = -orig.p_high,
//     domain.p_high = -orig.p_low), а PG хранит business-facing положительные
//     значения. Без sign-flip первый же mixed BUY+SELL батч давал NaN solver.
//
// Транзакционность:
//   * Read — одна транзакция, ORDER BY window_start, order_id (детерминизм).
//   * Write — одна транзакция на batch; для каждой заявки SELECT ... FOR UPDATE
//     блокирует row до конца транзакции (защита от race с другими writers).
// ============================================================================

#include "infra/postgres/postgres_flow_order_repository.hpp"

#include <algorithm>       // std::transform — для to_lower_ascii
#include <cctype>          // std::tolower — char classification
#include <cstdint>         // std::int64_t — для epoch seconds
#include <stdexcept>       // std::invalid_argument / std::runtime_error
#include <unordered_map>   // быстрый lookup order_id → index при загрузке legs
#include <utility>         // std::move — для perfect-forwarding

#include "domain/fill_math.hpp"                  // ComputeFilledVolumeUpdate (clamp + status)
#include "infra/postgres/decimal_conversion.hpp" // ParsePgNumeric / ToPgNumeric

namespace cex::matching::infra {

namespace {

// ----------------------------------------------------------------------------
// Время: PG хранит TIMESTAMPTZ, домен — std::chrono::system_clock::time_point.
// Через EPOCH-секунды как промежуточный обменный формат — простая, надёжная
// конвертация без таймзонной путаницы (UTC всегда).
// ----------------------------------------------------------------------------

/// time_point → секунды от epoch (Unix time). Truncation, не rounding.
std::int64_t to_epoch_seconds(std::chrono::system_clock::time_point time_point) {
  return std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch()).count();
}

/// Обратно: int64 секунды → time_point. Brace-init time_point напрямую из
/// duration — конструктор chrono::time_point<Clock, Duration>{Duration}.
std::chrono::system_clock::time_point from_epoch_seconds(std::int64_t epoch_seconds) {
  return std::chrono::system_clock::time_point{std::chrono::seconds{epoch_seconds}};
}

/// In-place lower-case ASCII normalize. Берёт string by value, мутирует копию,
/// возвращает — RVO/move оптимизирует возврат. unsigned char cast — UB-safe
/// invocation tolower (см. decimal_conversion.cpp). Лямбда возвращает char,
/// поэтому static_cast<char> обязателен (tolower returns int).
std::string to_lower_ascii(std::string input) {
  std::transform(input.begin(), input.end(), input.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return input;
}

// ----------------------------------------------------------------------------
// Парсеры enum-полей из PG text.
// ----------------------------------------------------------------------------

/// PG time_in_force → domain enum. Бросает invalid_argument при незнакомом
/// значении — это лучше, чем silent fallback (можно пропустить миграцию).
domain::FlowOrderTimeInForce parse_time_in_force(const std::string& db_tif) {
  const auto tif = to_lower_ascii(db_tif);
  if (tif == "gtc") {
    return domain::FlowOrderTimeInForce::kGtc;
  }
  if (tif == "gtd") {
    return domain::FlowOrderTimeInForce::kGtd;
  }
  if (tif == "ioc") {
    return domain::FlowOrderTimeInForce::kIoc;
  }
  throw std::invalid_argument("Unsupported flow_orders.time_in_force value: " + db_tif);
}

/// PG status → domain enum. Семь значений: new/active/partially_filled/filled/
/// cancelled/expired/liquidated. Любое другое = ошибка миграции.
domain::FlowOrderStatus parse_status(const std::string& db_status) {
  const auto status = to_lower_ascii(db_status);
  if (status == "new") {
    return domain::FlowOrderStatus::kNew;
  }
  if (status == "active") {
    return domain::FlowOrderStatus::kActive;
  }
  if (status == "partially_filled") {
    return domain::FlowOrderStatus::kPartiallyFilled;
  }
  if (status == "filled") {
    return domain::FlowOrderStatus::kFilled;
  }
  if (status == "cancelled") {
    return domain::FlowOrderStatus::kCancelled;
  }
  if (status == "expired") {
    return domain::FlowOrderStatus::kExpired;
  }
  if (status == "liquidated") {
    return domain::FlowOrderStatus::kLiquidated;
  }
  throw std::invalid_argument("Unsupported flow_orders.status value: " + db_status);
}

// ----------------------------------------------------------------------------
// map_flow_order — превращает одну row из SELECT в domain::FlowOrder.
// Legs (flow_order_legs) дозагружаются отдельным запросом ниже —
// здесь только колонки самой flow_orders.
// ----------------------------------------------------------------------------
/// Чтение row.column через row["name"].as<T>() — libpqxx-API. Все NUMERIC
/// приходят как ::text (см. SQL), парсятся ParsePgNumeric (PR-F02-002 strip).
domain::FlowOrder map_flow_order(const pqxx::row& row) {
  domain::FlowOrder order;
  order.order_id = row["order_id"].as<std::string>();
  order.user_id = row["user_id"].as<std::string>();
  order.p_low = postgres::ParsePgNumeric(row["p_low"].as<std::string>());
  order.p_high = postgres::ParsePgNumeric(row["p_high"].as<std::string>());
  order.q_rate = postgres::ParsePgNumeric(row["q_rate"].as<std::string>());
  order.q_max = postgres::ParsePgNumeric(row["q_max"].as<std::string>());
  order.filled_cum = postgres::ParsePgNumeric(row["filled_cum"].as<std::string>());
  order.time_in_force = parse_time_in_force(row["time_in_force"].as<std::string>());
  order.status = parse_status(row["status"].as<std::string>());

  // window_start — обязательное поле. window_end — nullable (GTC = window_end IS NULL).
  order.window_start = from_epoch_seconds(row["window_start_epoch"].as<std::int64_t>());
  if (row["window_end_epoch"].is_null()) {
    order.window_end = std::nullopt;   // std::optional<time_point>
  } else {
    order.window_end = from_epoch_seconds(row["window_end_epoch"].as<std::int64_t>());
  }

  order.created_at = from_epoch_seconds(row["created_at_epoch"].as<std::int64_t>());
  order.updated_at = from_epoch_seconds(row["updated_at_epoch"].as<std::int64_t>());
  return order;
}

// ----------------------------------------------------------------------------
// build_load_active_orders_query — динамически строит SELECT с опциональными
// фильтрами (instrument, limit). Использует raw-string literal R"SQL(...)SQL"
// для читаемости мульти-строчного запроса.
// ----------------------------------------------------------------------------
/// Параметры: $1 = as_of_epoch (double precision), $2 = instrument (если есть),
/// $3 = limit (если есть; иначе $2 при отсутствии instrument).
/// EXTRACT(EPOCH FROM ts)::bigint — нативный PG способ привести TIMESTAMPTZ
/// к int64-секундам, согласованный с to_epoch_seconds() выше.
std::string build_load_active_orders_query(bool has_instrument_filter, bool has_limit) {
  std::string sql = R"SQL(
SELECT
  fo.order_id::text AS order_id,
  fo.user_id::text AS user_id,
  fo.p_low::text AS p_low,
  fo.p_high::text AS p_high,
  fo.q_rate::text AS q_rate,
  fo.q_max::text AS q_max,
  fo.filled_cum::text AS filled_cum,
  fo.time_in_force::text AS time_in_force,
  fo.status::text AS status,
  EXTRACT(EPOCH FROM fo.window_start)::bigint AS window_start_epoch,
  CASE
    WHEN fo.window_end IS NULL THEN NULL
    ELSE EXTRACT(EPOCH FROM fo.window_end)::bigint
  END AS window_end_epoch,
  EXTRACT(EPOCH FROM fo.created_at)::bigint AS created_at_epoch,
  EXTRACT(EPOCH FROM fo.updated_at)::bigint AS updated_at_epoch
FROM flow_orders fo
WHERE
  fo.status IN ('active', 'partially_filled')
  AND fo.filled_cum < fo.q_max
  AND fo.window_start <= to_timestamp($1::double precision)
  AND (fo.window_end IS NULL OR to_timestamp($1::double precision) < fo.window_end)
  -- IOC orders are excluded from periodic batch-clearing by design.
  AND fo.time_in_force <> 'IOC'
)SQL";

  // Условный фильтр через EXISTS-subquery: проверяем, что у заявки есть нога
  // с искомым символом. EXISTS быстрее JOIN+DISTINCT для этого случая.
  if (has_instrument_filter) {
    sql += R"SQL(
  AND EXISTS (
    SELECT 1
    FROM flow_order_legs fol
    WHERE fol.order_id = fo.order_id
      AND fol.instrument_symbol = $2
  )
)SQL";
  }

  // ORDER BY window_start, order_id — детерминистичный порядок (важно для
  // replay-tests и debugging).
  sql += " ORDER BY fo.window_start, fo.order_id";
  if (has_limit) {
    // Номер параметра зависит от того, был ли $2 занят instrument-filter'ом.
    sql += has_instrument_filter ? " LIMIT $3" : " LIMIT $2";
  }
  return sql;
}

// ----------------------------------------------------------------------------
// build_load_legs_query — формирует SELECT с динамическим IN ($1, $2, ..., $N)
// для batch-загрузки ног по списку order_ids. PG не имеет native array-param
// для prepared statements в libpqxx, поэтому строим список плейсхолдеров вручную.
// ----------------------------------------------------------------------------
std::string build_load_legs_query(
    std::size_t order_ids_count) {
  std::string sql = R"SQL(
SELECT
  order_id::text AS order_id,
  instrument_symbol,
  weight::text AS weight
FROM flow_order_legs
WHERE order_id::text IN ()SQL";

  // Конкатенация "$1, $2, ..., $N". Не использует std::format чтобы избежать
  // ослабления C++20-availability (libstdc++ format поддержка пришла поздно).
  for (std::size_t i = 0; i < order_ids_count; ++i) {
    if (i > 0) {
      sql += ", ";
    }
    sql += "$";
    sql += std::to_string(i + 1);   // pqxx parameters 1-indexed
  }

  sql += ") ORDER BY order_id, instrument_symbol";
  return sql;
}

/// Проверка инвариантов после загрузки — бросает runtime_error если
/// flow_orders в неконсистентном состоянии (битая миграция / ручная правка).
void validate_loaded_order(const domain::FlowOrder& order) {
  if (!order.has_valid_legs()) {
    throw std::runtime_error("flow_orders has invalid legs for order_id=" + order.order_id);
  }
  if (!order.has_valid_fill_state()) {
    throw std::runtime_error("flow_orders has filled_cum > q_max for order_id=" + order.order_id);
  }
}

}  // namespace

// ----------------------------------------------------------------------------
// Конструкторы (DSN-form и factory-form) — те же паттерны, что в order_flow
// версии репо (см. cpp/order_flow/src/infra/postgres/.../.cpp).
// ----------------------------------------------------------------------------
PostgresFlowOrderRepository::PostgresFlowOrderRepository(std::string connection_string)
    : connection_factory_([conn_str = std::move(connection_string)]() {
        return std::make_unique<pqxx::connection>(conn_str);
      }) {}

PostgresFlowOrderRepository::PostgresFlowOrderRepository(ConnectionFactory connection_factory)
    : connection_factory_(std::move(connection_factory)) {
  if (!connection_factory_) {
    throw std::invalid_argument("PostgresFlowOrderRepository requires a valid connection factory");
  }
}

// ============================================================================
// LoadActiveFlowOrders — основной reader, вызывается на каждом тике matching.
//
// Алгоритм:
//   1. Открыть соединение, начать транзакцию.
//   2. Загрузить активные flow_orders (с фильтрами).
//   3. По собранному списку order_ids — загрузить flow_order_legs.
//   4. Распределить legs по соответствующим заявкам (через unordered_map index).
//   5. Для каждой заявки: sort_legs_by_symbol(), sign-flip для SELL
//      (PR-F02-005), validate_loaded_order.
//   6. Commit и return.
//
// Возвращает: vector<FlowOrder>, готовый к передаче в ContinuousClearingSolver.
// ============================================================================
std::vector<domain::FlowOrder> PostgresFlowOrderRepository::LoadActiveFlowOrders(
    std::chrono::system_clock::time_point as_of_time,
    const std::optional<std::string>& instrument_filter,
    const std::optional<std::int32_t>& limit) {
  // Валидация опциональных параметров — fail fast.
  if (limit.has_value() && *limit <= 0) {
    throw std::invalid_argument("LoadActiveFlowOrders limit must be positive");
  }
  if (instrument_filter.has_value() && instrument_filter->empty()) {
    throw std::invalid_argument("LoadActiveFlowOrders instrument_filter must not be empty");
  }

  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }

  const auto as_of_epoch = to_epoch_seconds(as_of_time);
  pqxx::work tx(*conn);
  const std::string sql =
      build_load_active_orders_query(instrument_filter.has_value(), limit.has_value());

  // Четыре формы вызова в зависимости от какие параметры заданы.
  // Альтернатива — std::vector<std::any> или std::tuple, но прямой if-else
  // понятнее и не платит за runtime polymorphism.
  pqxx::result rows;
  if (instrument_filter.has_value() && limit.has_value()) {
    rows = tx.exec_params(sql, as_of_epoch, *instrument_filter, *limit);
  } else if (instrument_filter.has_value()) {
    rows = tx.exec_params(sql, as_of_epoch, *instrument_filter);
  } else if (limit.has_value()) {
    rows = tx.exec_params(sql, as_of_epoch, *limit);
  } else {
    rows = tx.exec_params(sql, as_of_epoch);
  }

  // Pre-allocate vectors с capacity = rows.size() (rows.size() возвращает int,
  // reserve хочет size_t — implicit conversion безопасен здесь).
  std::vector<domain::FlowOrder> orders;
  orders.reserve(rows.size());
  std::vector<std::string> order_ids;
  order_ids.reserve(rows.size());
  // Inverse-index: order_id → позиция в orders vector. Нужен на этапе
  // присоединения legs ниже (иначе O(N*M) поиск).
  std::unordered_map<std::string, std::size_t> order_index_by_id;
  order_index_by_id.reserve(rows.size());

  // Первая фаза: маппинг row → FlowOrder, накопление order_ids и индекса.
  for (const auto& row : rows) {
    orders.push_back(map_flow_order(row));
    order_ids.push_back(orders.back().order_id);
    // emplace в map: ключ=order_id, значение=индекс только что добавленной
    // заявки в orders. orders.size()-1 — текущий индекс.
    order_index_by_id.emplace(orders.back().order_id, orders.size() - 1);
  }

  // Вторая фаза: загрузка ног batch-запросом IN (...).
  // make_dynamic_params — libpqxx-API для variadic-параметров из range.
  // Передаёт каждый order_id как отдельный $1, $2, ..., $N (см. build_load_legs_query).
  if (!order_ids.empty()) {
    const auto leg_rows = tx.exec_params(
        build_load_legs_query(order_ids.size()),
        pqxx::prepare::make_dynamic_params(order_ids.begin(), order_ids.end()));
    for (const auto& row : leg_rows) {
      const auto order_id = row["order_id"].as<std::string>();
      const auto index_it = order_index_by_id.find(order_id);
      if (index_it == order_index_by_id.end()) {
        // Это означает либо race (заявка ушла между двумя SELECT'ами в той же
        // транзакции — невозможно при snapshot-isolation), либо битая FK.
        throw std::runtime_error("flow_order_legs contains unknown order_id=" + order_id);
      }

      domain::FlowOrderLeg leg;
      leg.instrument_symbol = row["instrument_symbol"].as<std::string>();
      leg.weight = postgres::ParsePgNumeric(row["weight"].as<std::string>());
      // std::move — leg больше нигде не нужен, передаём ownership строки в vector.
      orders[index_it->second].legs.push_back(std::move(leg));
    }
  }

  // Третья фаза: post-processing каждой заявки.
  for (auto& order : orders) {
    order.sort_legs_by_symbol();
    // PR-F02-005 boundary mapping: PG holds business-facing prices
    // (p_low/p_high positive, constrained by `p_low > 0`), but the
    // matching domain convention — used by ContinuousClearingSolver and
    // ComputeInternalOverlapPrice — is signed by side: SELL legs carry
    // domain.p_low = -orig.p_high, domain.p_high = -orig.p_low (see
    // cpp/matching/src/domain/flow_order.hpp:127). The Kafka-fed path
    // does this in `FlowOrder::from_proto`; the PG-fed path was missing
    // it, so SELL orders loaded from PG broke the solver: pi was set
    // off-axis (e.g. -4000) and SolveImpl produced NaN within a few
    // iterations, collapsing fills to zero for ANY mixed BUY+SELL book.
    // For single-leg orders the side is determined by leg weight sign
    // (matches the writer in cpp/order_flow/.../postgres_flow_order_
    // repository.cpp). For multi-leg portfolio orders the per-leg sign
    // is more nuanced (F-09); we keep things consistent with the proto
    // path which sees one global `side` per order, so we use the first
    // leg's weight sign as the order-level side here. When F-09 lands,
    // this boundary should be revisited together with per-leg sign
    // handling in the solver.
    if (!order.legs.empty()) {
      const auto& first_leg = order.legs.front();
      const auto zero = cex::common::Decimal::zero();
      // weight < 0 = SELL. Cmp возвращает -1 → нужна инверсия знака цен.
      const bool is_sell =
          cex::common::Decimal::cmp(first_leg.weight, zero) < 0;
      if (is_sell) {
        const auto orig_p_low = order.p_low;
        const auto orig_p_high = order.p_high;
        // sub(zero, x) = -x — отрицание Decimal через явное вычитание.
        // Также swap: было [low, high] = [99, 101] → стало [-101, -99].
        order.p_low = cex::common::Decimal::sub(zero, orig_p_high);
        order.p_high = cex::common::Decimal::sub(zero, orig_p_low);
      }
    }
    validate_loaded_order(order);
  }

  tx.commit();
  return orders;
}

// ============================================================================
// UpdateFilledVolumes — writer для filled_cum после batch-clearing.
//
// Алгоритм для каждой delta:
//   1. SELECT q_max, filled_cum FOR UPDATE — блокирует row.
//   2. ComputeFilledVolumeUpdate(q_max, prev, delta) — domain логика clamp+status.
//   3. UPDATE с новым filled_cum и статусом 'filled' или 'partially_filled'.
//
// Один tx на весь batch — atomicity: либо все deltas применились, либо ни одна.
// ============================================================================
void PostgresFlowOrderRepository::UpdateFilledVolumes(
    const std::vector<domain::OrderFillDelta>& deltas,
    std::chrono::system_clock::time_point batch_time) {
  // Пустой batch — ничего не делаем (избегаем connection churn).
  if (deltas.empty()) {
    return;
  }

  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }

  const auto batch_epoch = to_epoch_seconds(batch_time);
  pqxx::work tx(*conn);

  for (const auto& delta : deltas) {
    if (delta.order_id.empty()) {
      throw std::invalid_argument("OrderFillDelta.order_id must not be empty");
    }

    // Пропускаем delta=0 — не нужно тратить SELECT FOR UPDATE на no-op.
    // Семантика: если solver вернул ноль исполнения для заявки, статус
    // остаётся прежним.
    const cex::common::Decimal zero{0, delta.executed_qty_delta.scale};
    if (cex::common::Decimal::cmp(delta.executed_qty_delta, zero) <= 0) {
      continue;
    }

    // SELECT ... FOR UPDATE — pessimistic lock. Блокирует row до tx.commit(),
    // не давая параллельным UPDATE/SELECT FOR UPDATE прочитать stale данные.
    // Обязательно для read-modify-write последовательности.
    const auto locked_rows = tx.exec_params(R"SQL(
SELECT
  q_max::text AS q_max,
  filled_cum::text AS filled_cum
FROM flow_orders
WHERE order_id = $1
FOR UPDATE
)SQL",
                                            delta.order_id);

    if (locked_rows.empty()) {
      // Заявка пропала между batch-solve и UpdateFilledVolumes — потенциально
      // baseline-нарушение (manual DELETE? миграция в процессе?).
      throw std::runtime_error("flow_orders row not found for order_id=" + delta.order_id);
    }

    const auto& row = locked_rows.front();
    const auto q_max = postgres::ParsePgNumeric(row["q_max"].as<std::string>());
    const auto current_filled = postgres::ParsePgNumeric(row["filled_cum"].as<std::string>());

    // Domain-функция: clamp(prev + delta, q_max) + статус-резолвинг.
    // См. fill_math.hpp — единственный источник истины для этой математики.
    const auto update = domain::ComputeFilledVolumeUpdate(
        q_max, current_filled, delta.executed_qty_delta);

    // Две ветки UPDATE — отличаются только статусом ('filled' vs
    // 'partially_filled'). Не объединяем через CASE из-за читаемости PG-логов.
    if (update.new_status == domain::FlowOrderStatus::kFilled) {
      tx.exec_params(R"SQL(
UPDATE flow_orders
SET
  filled_cum = $1::numeric,
  status = 'filled',
  updated_at = to_timestamp($2::double precision)
WHERE order_id = $3
)SQL",
                     postgres::ToPgNumeric(update.new_filled_cum),
                     batch_epoch,
                     delta.order_id);
    } else {
      tx.exec_params(R"SQL(
UPDATE flow_orders
SET
  filled_cum = $1::numeric,
  status = 'partially_filled',
  updated_at = to_timestamp($2::double precision)
WHERE order_id = $3
)SQL",
                     postgres::ToPgNumeric(update.new_filled_cum),
                     batch_epoch,
                     delta.order_id);
    }
  }

  // Atomic commit. Если бросит — RAII tx откатит всю серию UPDATE'ов.
  tx.commit();
}

}  // namespace cex::matching::infra
