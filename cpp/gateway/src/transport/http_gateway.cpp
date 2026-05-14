// =============================================================================
// Реализация HTTP-маршрутов шлюза. Crow собирает запросы в crow::request,
// тут они парсятся, конвертируются в proto и отправляются дальше.
// =============================================================================

#include "transport/http_gateway.hpp"

#include <cmath>
#include <sstream>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

#include "crow.h"

namespace cex::gateway::transport {

// Преобразование "человеческого" double в Decimal с заданным scale.
// Пример: x=12.34, scale=2 -> units=1234.
// ВАЖНО: double-арифметика теряет точность. Это приемлемо только в шлюзе на
// границе с UI; внутри сервисов всегда используется fixed-point Decimal.
static fob::common::v1::Decimal dec_from_double(double x, int32_t scale) {
  double factor = std::pow(10.0, static_cast<double>(scale));
  int64_t units = static_cast<int64_t>(std::llround(x * factor));
  fob::common::v1::Decimal d;
  d.set_units(units);
  d.set_scale(scale);
  return d;
}

// Удобные хелперы для извлечения полей из crow::json::rvalue с дефолтами,
// чтобы не падать на неполных запросах (UI часто не присылает что-то).

static bool has(const crow::json::rvalue& v, const char* key) {
  return v.has(key) && v[key].t() != crow::json::type::Null;
}

// Достаёт строку. Если поле — число, конвертирует в строку (помогает с id).
static std::string get_string(const crow::json::rvalue& v, const char* key, const std::string& def = "") {
  if (!has(v, key)) return def;
  if (v[key].t() == crow::json::type::String) return std::string(v[key].s());
  std::ostringstream oss;
  oss << v[key].d();
  return oss.str();
}

// Достаёт double. Поддерживает и числа, и строки ("12.5").
static double get_double(const crow::json::rvalue& v, const char* key) {
  if (!has(v, key)) return 0.0;
  if (v[key].t() == crow::json::type::Number) return v[key].d();
  if (v[key].t() == crow::json::type::String) return std::stod(std::string(v[key].s()));
  return 0.0;
}

// Парсинг side из строки. Допускаем "buy"/"b"/"sell"/"s", регистро-независимо.
static fob::common::v1::Side parse_side(const std::string& s) {
  std::string x = s;
  for (auto& c : x) c = static_cast<char>(std::tolower(c));
  if (x=="buy"  || x=="b") return fob::common::v1::SIDE_BUY;
  if (x=="sell" || x=="s") return fob::common::v1::SIDE_SELL;
  return fob::common::v1::SIDE_UNSPECIFIED;
}

// Разбор символа пары "BTC/USDT" -> base="BTC", quote="USDT".
// Если разделителя нет — оставляем base/quote пустыми, доменный слой это отвалидирует.
static fob::common::v1::Instrument parse_instrument(const std::string& symbol) {
  fob::common::v1::Instrument inst;
  inst.set_symbol(symbol);

  auto pos = symbol.find('/');
  if (pos != std::string::npos) {
    inst.set_base(symbol.substr(0, pos));
    inst.set_quote(symbol.substr(pos + 1));
  }
  return inst;
}

HttpGateway::HttpGateway(infra::OrderFlowClient client)
    : order_flow_(std::move(client)) {}

void HttpGateway::run(uint16_t port) {
  crow::SimpleApp app;

  // Liveness/readiness — нужны Kubernetes-пробам.
  CROW_ROUTE(app, "/healthz")([] { return crow::response(200, "ok"); });
  CROW_ROUTE(app, "/readyz")([]  { return crow::response(200, "ready"); });

  // POST /v1/flow-orders — основной публичный endpoint MVP.
  CROW_ROUTE(app, "/v1/flow-orders").methods(crow::HTTPMethod::Post)(
      [this](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) {
          return crow::response(400, "Invalid JSON");
        }

        // ----- 1) Собираем FlowOrder (proto) -----
        fob::orders::v1::FlowOrder order;
        order.set_order_id(cex::common::uuid_v4());
        order.set_client_order_id(get_string(body, "client_order_id", ""));
        order.set_user_id(get_string(body, "user_id", "demo-user"));
        order.set_account_id(get_string(body, "account_id", "demo-account"));

        std::string symbol = get_string(body, "symbol", "BTC/USDT");
        *order.mutable_instrument() = parse_instrument(symbol);

        order.set_side(parse_side(get_string(body, "side", "buy")));

        // Дефолтные scale: для qty=8 (как BTC), для price=2 (как USDT).
        // В проде это должно браться из метаданных инструмента (tick/lot size).
        *order.mutable_total_qty()     = dec_from_double(get_double(body, "total_qty"), 8);
        *order.mutable_remaining_qty() = order.total_qty();

        *order.mutable_price_low()  = dec_from_double(get_double(body, "price_low"),  2);
        *order.mutable_price_high() = dec_from_double(get_double(body, "price_high"), 2);

        *order.mutable_max_speed() = dec_from_double(get_double(body, "max_speed"), 8);

        order.set_status(fob::common::v1::ORDER_STATUS_NEW);
        *order.mutable_created_at() = cex::common::now_ts();
        *order.mutable_updated_at() = cex::common::now_ts();

        // ----- 2) Готовим gRPC-обёртку с meta-данными -----
        fob::orders::v1::CreateFlowOrderRequest grpc_req;
        auto* meta = grpc_req.mutable_meta();
        meta->set_event_id(cex::common::uuid_v4());
        *meta->mutable_ts_event() = cex::common::now_ts();
        meta->set_source("gateway");
        meta->set_correlation_id(cex::common::uuid_v4());
        // Для шлюза партиционируем по user — общая трасса по одному пользователю остаётся последовательной.
        meta->set_partition_key(order.user_id());

        *grpc_req.mutable_order() = order;

        // ----- 3) Зовём OrderFlowService -----
        auto grpc_resp = order_flow_.CreateFlowOrder(grpc_req);

        // ----- 4) Маппим обратно в JSON для UI -----
        crow::json::wvalue out;
        out["accepted"] = grpc_resp.accepted();
        out["order_id"] = grpc_resp.order_id();
        if (grpc_resp.has_error()) {
          out["error"]["code"]    = grpc_resp.error().code();
          out["error"]["message"] = grpc_resp.error().message();
        }
        return crow::response(200, out);
      });

  cex::common::log_json("INFO", "Gateway starting", {{"port", std::to_string(port)}});
  // multithreaded() — Crow поднимет worker-потоки. run() блокирует.
  app.port(port).multithreaded().run();
}

}  // namespace cex::gateway::transport
