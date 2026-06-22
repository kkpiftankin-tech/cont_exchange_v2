// ============================================================================
// postgres_flow_order_repository.cpp — PostgreSQL writer для принятых FlowOrder.
//
// Назначение:
//   После того как order_flow.uc прошёл pre-trade risk + reserve funds в
//   ledger, заявка должна стать видимой для matching-сервиса. Matching читает
//   активные заявки из таблиц flow_orders + flow_order_legs (см. matching's
//   PostgresFlowOrderRepository::LoadActiveOrders). Этот класс отвечает за
//   правильную запись: status='active' и leg.weight=+1 для BUY / -1 для SELL.
//
// История появления (PR-F02-001, ~2026-06-01):
//   До этого репо order_flow держал заявки только в in-memory map + Kafka.
//   Matching в PG-режиме (MATCHING_POSTGRES_DSN) видел пустую таблицу
//   flow_orders → формировал 0 батчей при наличии активного flow.
//   Связка: ORDER_FLOW_POSTGRES_DSN + MATCHING_POSTGRES_DSN — обе должны
//   быть либо заданы, либо пусты (иначе race-condition по источнику истины).
//
// Дисциплина:
//   * Insert делается ON CONFLICT DO NOTHING — идемпотентность по order_id
//     (см. CLAUDE.md §8.2 инвариант 11: команды должны быть идемпотентными).
//   * Транзакция — pqxx::work — оборачивает оба INSERT'а: либо оба, либо
//     ничего (atomicity для writer + reader контракта).
//   * Decimal проходит через канонический to_string() — никаких double.
// ============================================================================

#include "infra/postgres/postgres_flow_order_repository.hpp"

#include <stdexcept>   // std::invalid_argument / std::runtime_error для validation
#include <utility>     // std::move для perfect-forwarding конструкторных аргументов

#include "cex/common/decimal.hpp"            // мост proto.Decimal → string для NUMERIC
#include "fob/common/v1/common.pb.h"         // generated proto: TimeInForce, Side, Decimal

namespace cex::order_flow::infra {

namespace {  // anonymous namespace: internal-linkage utility функции

// Map proto TimeInForce -> flow_orders.time_in_force ('GTC'|'GTD'|'IOC').
// Matching's batch SELECT excludes IOC explicitly; everything else is
// included. Unspecified/FOK fall back to GTC (the most permissive value)
// because batch clearing is the only execution path right now.
//
/// Преобразует enum TimeInForce в строковый код для колонки time_in_force.
/// IOC → "IOC" (matching отфильтрует — IOC исполняется в первом же батче или
/// отменяется); всё остальное → "GTC" (good-til-cancelled, висит до явного
/// cancel). FOK/UNSPECIFIED маппятся в GTC, потому что batch-clearing —
/// единственный путь исполнения, и FOK-semantics там не имеет смысла.
std::string time_in_force_to_db(fob::common::v1::TimeInForce tif) {
  // switch на enum даёт компилятору warning, если добавили новый case —
  // полезно как safety-net при эволюции proto.
  switch (tif) {
    case fob::common::v1::TIF_IOC:
      return "IOC";
    case fob::common::v1::TIF_GTC:
    case fob::common::v1::TIF_FOK:
    case fob::common::v1::TIF_UNSPECIFIED:
    default:
      // Fall-through на default охватывает любое будущее значение enum, чтобы
      // случайное TIF_GTD не сломало INSERT (PG отвергнет неизвестное значение
      // CHECK-constraint'ом).
      return "GTC";
  }
}

// flow_order_legs.weight convention from matching domain:
//   BUY  ->  +1
//   SELL ->  -1
// See cpp/matching/src/domain/flow_order.hpp:116.
//
/// Маппинг направления на знак-вес ноги (leg) портфельной заявки. В matching
/// domain каждая заявка — это вектор объёмов по инструментам; для одной
/// инструментальной FlowOrder вектор имеет одну ногу с weight=±1.
/// Знак критичен для solver: BUY (+1) увеличивает базовую позицию,
/// SELL (-1) уменьшает.
std::string side_to_leg_weight(fob::common::v1::Side side) {
  if (side == fob::common::v1::SIDE_SELL) return "-1";
  return "1";   // SIDE_BUY и UNSPECIFIED → +1 (BUY-семантика по умолчанию)
}

/// Конвертирует proto.Decimal в текстовое представление для PG NUMERIC.
/// Делегирует двум canonical-функциям:
///   1. Decimal::from_proto() — proto.Decimal {units, scale} → cex::common::Decimal
///   2. Decimal::to_string() — "[-]int.frac" без trailing zeros
/// libpqxx сам экранирует строку при подстановке в $N — безопасно.
std::string decimal_to_pg(const fob::common::v1::Decimal& proto_decimal) {
  return cex::common::Decimal::from_proto(proto_decimal).to_string();
}

}  // namespace

// ----------------------------------------------------------------------------
// Конструкторы. Класс поддерживает две формы инициализации:
//   1. Из готовой DSN-строки — самый простой случай для prod.
//   2. Из ConnectionFactory (std::function) — для тестов с моком соединения.
// ----------------------------------------------------------------------------

/// Конструктор по DSN. Захватывает строку в лямбду через by-value capture,
/// которая создаёт новое соединение на каждый InsertFlowOrder вызов.
/// std::move() передаёт строку в capture без копирования (move-construct).
PostgresFlowOrderRepository::PostgresFlowOrderRepository(std::string connection_string)
    : connection_factory_([conn_str = std::move(connection_string)]() {
        // C++14 init-capture: создаём conn_str внутри лямбды как member.
        // make_unique<pqxx::connection> — RAII обёртка; деструктор закрывает
        // соединение автоматически.
        return std::make_unique<pqxx::connection>(conn_str);
      }) {}

/// Конструктор по фабрике (для тестов). Бросает invalid_argument на nullptr,
/// потому что отложенный crash в InsertFlowOrder() гораздо хуже сразу же.
PostgresFlowOrderRepository::PostgresFlowOrderRepository(ConnectionFactory connection_factory)
    : connection_factory_(std::move(connection_factory)) {
  if (!connection_factory_) {  // std::function::operator bool — false для пустой
    throw std::invalid_argument(
        "PostgresFlowOrderRepository requires a valid connection factory");
  }
}

// ----------------------------------------------------------------------------
// InsertFlowOrder — единственный публичный метод. Пишет flow_orders +
// flow_order_legs в одной транзакции.
// ----------------------------------------------------------------------------
void PostgresFlowOrderRepository::InsertFlowOrder(
    const fob::orders::v1::FlowOrder& order) {
  // Pre-checks обязательных полей. Лучше отказать сразу, чем писать невалидный
  // row и потом получать FK-violation от matching при загрузке.
  if (order.order_id().empty()) {
    throw std::invalid_argument("InsertFlowOrder: order_id is empty");
  }
  if (order.user_id().empty()) {
    throw std::invalid_argument("InsertFlowOrder: user_id is empty");
  }
  if (order.instrument().symbol().empty()) {
    throw std::invalid_argument("InsertFlowOrder: instrument.symbol is empty");
  }

  // Открываем соединение через фабрику. unique_ptr — auto-cleanup при выходе.
  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }

  // pqxx::work — read-write транзакция с auto-rollback в деструкторе,
  // если не вызвать commit(). Идеально для exception-safety.
  pqxx::work tx(*conn);

  // Status = 'active': matching's PostgresFlowOrderRepository::LoadActive...
  // filters status IN ('active','partially_filled'). Writing 'new' here would
  // make accepted orders invisible to F-04 batch clearing (the bug fixed by
  // this commit).
  //
  // Raw-string literal R"SQL(...)SQL" позволяет не экранировать кавычки и
  // переносы строк внутри SQL. Делает мульти-строчные запросы читаемыми.
  // ON CONFLICT (order_id) DO NOTHING — идемпотентность: повторная подача
  // того же CreateFlowOrder не создаёт дубля и не падает с unique-violation.
  // $N::numeric — явный cast string → NUMERIC, потому что libpqxx передаёт
  // Decimal как text-param (нет нативного NUMERIC binding в protocol).
  tx.exec_params(R"SQL(
INSERT INTO flow_orders (
  order_id, user_id, p_low, p_high, q_rate, q_max,
  filled_cum, time_in_force, status,
  window_start, window_end
) VALUES (
  $1::uuid, $2, $3::numeric, $4::numeric, $5::numeric, $6::numeric,
  0, $7, 'active',
  NOW(), NULL
) ON CONFLICT (order_id) DO NOTHING
)SQL",
                 order.order_id(),                       // $1: UUID
                 order.user_id(),                        // $2: text (account owner)
                 decimal_to_pg(order.price_low()),       // $3: NUMERIC нижняя граница
                 decimal_to_pg(order.price_high()),      // $4: NUMERIC верхняя граница
                 decimal_to_pg(order.max_speed()),       // $5: q_rate (max base/sec)
                 decimal_to_pg(order.total_qty()),       // $6: q_max (max total base)
                 time_in_force_to_db(order.tif()));      // $7: 'IOC'|'GTC'

  // Вторая INSERT — нога заявки (leg). В текущей одно-инструментальной модели
  // ровно одна leg на заявку. weight определяет знак (BUY=+1, SELL=-1) для
  // matching solver. Composite PK (order_id, instrument_symbol) — ON CONFLICT
  // DO NOTHING защищает от дублей при retry.
  tx.exec_params(R"SQL(
INSERT INTO flow_order_legs (order_id, instrument_symbol, weight)
VALUES ($1::uuid, $2, $3::numeric)
ON CONFLICT (order_id, instrument_symbol) DO NOTHING
)SQL",
                 order.order_id(),                       // $1: FK на flow_orders
                 order.instrument().symbol(),            // $2: "BTC/USDT"
                 side_to_leg_weight(order.side()));      // $3: "+1"|"-1"

  // Атомарный commit. Если бросит — RAII откатит транзакцию автоматически.
  // После успешного commit matching на следующем тике увидит запись.
  tx.commit();
}

}  // namespace cex::order_flow::infra
