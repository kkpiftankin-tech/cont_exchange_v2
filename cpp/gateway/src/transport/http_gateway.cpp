// ============================================================================
// http_gateway.cpp — HTTP REST endpoints для gateway сервиса.
//
// Назначение:
//   Внешний REST API для UI и операторов. Routes:
//
//   F-02 (Create FlowOrder):
//     POST /v1/flow-orders                — proxy → order_flow gRPC.
//     POST /v1/flow-orders/:id/cancel     — proxy → order_flow gRPC.
//     GET  /v1/flow-orders/:id             — proxy → order_flow gRPC.
//     GET  /v1/flow-orders                  — list (по user_id из auth).
//
//   F-09 (Combo Orders):
//     POST /v1/combo-orders                  — proxy → order_flow CreateCombo.
//     POST /v1/combo-orders/:id/cancel       — proxy → order_flow CancelCombo.
//
//   F-15 (Backtest / Replay):
//     POST /v1/replay/sessions               — create replay session.
//     POST /v1/replay/sessions/:id/cancel    — cancel.
//     POST /v1/replay/sessions/:id/retry     — retry failed session.
//     GET  /v1/replay/sessions/:id           — get session info.
//     GET  /v1/replay/sessions/:id/compare   — compare 2 sessions (parity).
//     SSE/long-poll /v1/replay/sessions/:id/results — streaming events.
//
//   Health:
//     GET /healthz                            — liveness probe.
//
// Auth middleware:
//   transport/auth_middleware.hpp — JWT / header-based. CLAUDE.md §22:
//   production требует mTLS termination перед gateway.
//
// CLAUDE.md §10 layering: gateway не имеет бизнес-логики — только marshalling
// HTTP/JSON ↔ gRPC. Все use cases живут в order_flow/backtest/etc.
// ============================================================================

#include "transport/http_gateway.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "cex/common/replay_kafka.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

#include "transport/auth_middleware.hpp"

#include "crow.h"

namespace cex::gateway::transport {

namespace {

using json = nlohmann::json;

/// Текущее время в epoch milliseconds — используется в audit JSON-полях
/// (created_at_ms и т.п.).
int64_t now_millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/// Canonical error response: {"error":{"code":...,"message":...}}.
/// HTTP status + JSON body — оба согласованы. Используется во всех handler'ах.
crow::response json_error(int status,
                          const std::string& code,
                          const std::string& message) {
  crow::json::wvalue out;
  out["errorcode"] = code;
  out["errordetails"] = message;
  return crow::response(status, out);
}

crow::json::wvalue replay_result_summary_json(
    const cex::common::ReplayResultMessage& message) {
  crow::json::wvalue out;
  out["sessionid"] = message.session_id;
  out["avgis"] = message.avg_is;
  out["totalpnl"] = message.total_pnl;
  out["avgpnl"] = message.avg_pnl;
  out["stdpnl"] = message.std_pnl;
  out["sharpe"] = message.sharpe;
  out["fillrate"] = message.avg_fill_rate;
  out["avgvwap"] = message.avg_vwap;
  out["avgsolvetime"] = message.avg_solve_time_ms;
  out["maxdrawdown"] = message.max_drawdown;
  out["totalbatches"] = message.total_batches;
  out["totalfillevents"] = message.total_fill_events;
  out["processedbatches"] = message.processed_batches;
  out["failedbatches"] = message.failed_batches;
  out["partial"] = message.partial;
  out["avgis_rule"] = message.avgis_rule;
  out["decision_price_source"] = message.decision_price_source;
  out["no_requested_volume"] = message.no_requested_volume;
  if (!message.error_details.empty()) {
    out["errordetails"] = message.error_details;
  }
  if (!message.error_code.empty()) {
    out["errorcode"] = message.error_code;
  }
  return out;
}

std::optional<std::string> json_string(const json& body,
                                       const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (body.contains(key) && body[key].is_string() &&
        !body[key].get<std::string>().empty()) {
      return body[key].get<std::string>();
    }
  }
  return std::nullopt;
}

bool has_object_or_id(const json& body,
                      const std::initializer_list<const char*> id_keys,
                      const std::initializer_list<const char*> object_keys) {
  if (json_string(body, id_keys).has_value()) return true;
  for (const char* key : object_keys) {
    if (body.contains(key) && body[key].is_object()) return true;
  }
  return false;
}

bool has_instruments(const json& body) {
  if (body.contains("instrument") && body["instrument"].is_string() &&
      !body["instrument"].get<std::string>().empty()) {
    return true;
  }
  if (!body.contains("instruments") || !body["instruments"].is_array() ||
      body["instruments"].empty()) {
    return false;
  }
  for (const auto& instrument : body["instruments"]) {
    if (instrument.is_string() && !instrument.get<std::string>().empty()) {
      return true;
    }
  }
  return false;
}

std::string normalize_reward_mode(std::string raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char ch : raw) {
    if (ch == '_' || ch == '-' || std::isspace(ch) != 0) continue;
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

bool is_supported_reward_mode(const std::string& raw) {
  const std::string mode = normalize_reward_mode(raw);
  return mode == "incrementalpnl" || mode == "is" ||
         mode == "negativeis" || mode == "minusis" ||
         mode == "shortfall";
}

/// Валидация reward_mode из replay session create payload.
/// Допустимые: "pnl", "vwap", "fill_rate". CASE-insensitive, normalized.
bool validate_reward_mode_payload(const json& body, std::string* error) {
  for (const char* key : {"rewardmode", "reward_mode"}) {
    if (body.contains(key) && !body[key].is_null() && !body[key].is_string()) {
      *error = std::string(key) + " must be a string";
      return false;
    }
  }
  auto explicit_mode = json_string(body, {"rewardmode", "reward_mode"});
  if (explicit_mode.has_value() && !is_supported_reward_mode(*explicit_mode)) {
    *error = "rewardmode must be incrementalPnL or -IS";
    return false;
  }
  for (const char* key : {"rewardconfig", "reward_config"}) {
    if (!body.contains(key) || body[key].is_null()) continue;
    if (!body[key].is_object()) {
      *error = std::string(key) + " must be a JSON object";
      return false;
    }
    for (const char* mode_key : {"mode", "rewardmode", "reward_mode"}) {
      if (body[key].contains(mode_key) && !body[key][mode_key].is_null() &&
          !body[key][mode_key].is_string()) {
        *error = std::string(key) + "." + mode_key + " must be a string";
        return false;
      }
    }
    auto reward_config_mode = json_string(body[key], {"mode", "rewardmode", "reward_mode"});
    if (reward_config_mode.has_value() &&
        !is_supported_reward_mode(*reward_config_mode)) {
      *error = std::string(key) + ".mode must be incrementalPnL or -IS";
      return false;
    }
  }
  return true;
}

bool is_blank_string(const std::string& value) {
  return value.empty() ||
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isspace(c) != 0;
         });
}

bool require_number(const json& object,
                    const char* key,
                    const std::string& field,
                    std::string* error) {
  if (!object.contains(key) || !object[key].is_number()) {
    *error = field + "." + key + " must be a number";
    return false;
  }
  return true;
}

/// Generic JSON validator: field в body должен быть числом > 0.
/// Используется для валидации qty/price/notional полей.
bool validate_positive_number(const json& object,
                              const char* key,
                              const std::string& field,
                              const bool required,
                              const bool allow_zero,
                              std::string* error) {
  if (!object.contains(key) || object[key].is_null()) {
    if (required) *error = field + "." + key + " is required";
    return !required;
  }
  if (!object[key].is_number()) {
    *error = field + "." + key + " must be a number";
    return false;
  }
  const double value = object[key].get<double>();
  if (!std::isfinite(value) || (allow_zero ? value < 0.0 : value <= 0.0)) {
    *error = field + "." + key + (allow_zero ? " must be >= 0" : " must be > 0");
    return false;
  }
  return true;
}

bool validate_fee_model_object(const json& model,
                               const std::string& field,
                               std::string* error) {
  if (!model.is_object()) {
    *error = field + " must be a JSON object";
    return false;
  }
  if (model.value("production", false)) return true;
  return validate_positive_number(model, "makerfeerate", field, true, true, error) &&
         validate_positive_number(model, "takerfeerate", field, true, true, error);
}

bool validate_solver_config_object(const json& solver,
                                   const std::string& field,
                                   std::string* error) {
  if (!solver.is_object()) {
    *error = field + " must be a JSON object";
    return false;
  }
  if (!validate_positive_number(solver, "batchintervalms", field, true, false, error)) {
    return false;
  }
  if (!validate_positive_number(solver, "maxiterations", field, true, false, error)) {
    return false;
  }
  if (!validate_positive_number(solver, "tolerance", field, true, false, error)) {
    return false;
  }
  if (!validate_positive_number(solver, "epsilonliquidity", field, true, true, error)) {
    return false;
  }
  if (solver.contains("feemodel") && !solver["feemodel"].is_null()) {
    if (!solver["feemodel"].is_object()) {
      *error = field + ".feemodel must be a JSON object";
      return false;
    }
    return validate_fee_model_object(solver["feemodel"], field + ".feemodel", error);
  }
  return true;
}

bool validate_risk_limits_object(const json& limits,
                                 const std::string& field,
                                 std::string* error) {
  if (!limits.is_object()) {
    *error = field + " must be a JSON object";
    return false;
  }
  for (const char* key : {"maxnotional", "maxposition", "maxleverage",
                         "maxorderrate"}) {
    if (!validate_positive_number(limits, key, field, true, false, error)) {
      return false;
    }
  }
  if (limits.contains("whitelist") && !limits["whitelist"].is_null()) {
    if (!limits["whitelist"].is_array()) {
      *error = field + ".whitelist must be an array";
      return false;
    }
    for (size_t i = 0; i < limits["whitelist"].size(); ++i) {
      if (!limits["whitelist"][i].is_string() ||
          is_blank_string(limits["whitelist"][i].get<std::string>())) {
        *error = field + ".whitelist[" + std::to_string(i) +
                 "] must be a non-empty string";
        return false;
      }
    }
  }
  return true;
}

bool validate_strategy_array(const json& strategy, std::string* error) {
  if (!strategy.is_array() || strategy.empty()) {
    *error = "strategy must be a non-empty FlowOrder JSON array";
    return false;
  }
  for (size_t i = 0; i < strategy.size(); ++i) {
    const std::string field = "strategy[" + std::to_string(i) + "]";
    if (!strategy[i].is_object()) {
      *error = field + " must be a JSON object";
      return false;
    }
    const auto& flow = strategy[i];
    if (!flow.contains("symbol") || !flow["symbol"].is_string() ||
        is_blank_string(flow["symbol"].get<std::string>())) {
      *error = field + ".symbol is required";
      return false;
    }
    if (!flow.contains("side") || !flow["side"].is_string()) {
      *error = field + ".side is required";
      return false;
    }
    std::string side = flow["side"].get<std::string>();
    std::transform(side.begin(), side.end(), side.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (side != "buy" && side != "sell") {
      *error = field + ".side must be buy or sell";
      return false;
    }
    if (!require_number(flow, "pL", field, error) ||
        !require_number(flow, "pH", field, error) ||
        !require_number(flow, "qrate", field, error) ||
        !require_number(flow, "qmax", field, error)) {
      return false;
    }
    if (flow["pL"].get<double>() > flow["pH"].get<double>()) {
      *error = field + ".pL must be <= pH";
      return false;
    }
    if (!validate_positive_number(flow, "qrate", field, true, false, error) ||
        !validate_positive_number(flow, "qmax", field, true, false, error)) {
      return false;
    }
    if (flow.contains("executionwindow") && !flow["executionwindow"].is_null() &&
        !validate_positive_number(flow, "executionwindow", field, true, false, error)) {
      return false;
    }
  }
  return true;
}

std::optional<int64_t> comparable_date(const json& value) {
  try {
    if (value.is_number_integer() || value.is_number_unsigned()) {
      return value.get<int64_t>();
    }
    if (value.is_string()) {
      const std::string raw = value.get<std::string>();
      if (raw.empty()) return std::nullopt;
      if (std::all_of(raw.begin(), raw.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
          })) {
        return std::stoll(raw);
      }
      if (raw.size() >= 10 && raw[4] == '-' && raw[7] == '-') {
        return std::stoll(raw.substr(0, 4) + raw.substr(5, 2) + raw.substr(8, 2));
      }
    }
  } catch (...) {
    return std::nullopt;
  }
  return std::nullopt;
}

bool validate_replay_create_payload(const json& body, std::string* error) {
  if (!json_string(body, {"name"}).has_value()) {
    *error = "name is required";
    return false;
  }
  if (!has_instruments(body)) {
    *error = "Instrument(s) must not be empty";
    return false;
  }
  if (!body.contains("daterangefrom") || !body.contains("daterangeto")) {
    *error = "daterangefrom and daterangeto are required";
    return false;
  }
  const auto from = comparable_date(body["daterangefrom"]);
  const auto to = comparable_date(body["daterangeto"]);
  if (!from.has_value() || !to.has_value()) {
    *error = "daterangefrom and daterangeto must be valid dates";
    return false;
  }
  if (from.has_value() && to.has_value() && *from > *to) {
    *error = "daterangefrom must be <= daterangeto";
    return false;
  }
  if (!body.contains("strategy") || !validate_strategy_array(body["strategy"], error)) {
    return false;
  }
  if (!validate_reward_mode_payload(body, error)) {
    return false;
  }
  if (!has_object_or_id(body, {"solverconfigid", "solver_config_id"},
                        {"solverconfig", "solver_config"})) {
    *error = "solverconfigid or inline solverconfig is required";
    return false;
  }
  if (!has_object_or_id(body, {"risklimitsid", "risk_limits_id"},
                        {"risklimits", "risk_limits"})) {
    *error = "risklimitsid or inline risklimits is required";
    return false;
  }
  if (!has_object_or_id(body, {"feemodelid", "fee_model_id"},
                        {"feemodel", "fee_model"})) {
    *error = "feemodelid or inline feemodel is required";
    return false;
  }
  if (body.contains("solverconfig") &&
      !validate_solver_config_object(body["solverconfig"], "solverconfig", error)) {
    return false;
  }
  if (body.contains("solver_config") &&
      !validate_solver_config_object(body["solver_config"], "solver_config", error)) {
    return false;
  }
  if (body.contains("risklimits") &&
      !validate_risk_limits_object(body["risklimits"], "risklimits", error)) {
    return false;
  }
  if (body.contains("risk_limits") &&
      !validate_risk_limits_object(body["risk_limits"], "risk_limits", error)) {
    return false;
  }
  if (body.contains("feemodel") &&
      !validate_fee_model_object(body["feemodel"], "feemodel", error)) {
    return false;
  }
  if (body.contains("fee_model") &&
      !validate_fee_model_object(body["fee_model"], "fee_model", error)) {
    return false;
  }
  return true;
}

std::string request_user_id(const crow::request& req,
                            const std::shared_ptr<AuthMiddleware>& auth_middleware,
                            const json& body) {
  if (auth_middleware != nullptr) {
    auto user = auth_middleware->ExtractUser(req);
    if (user.IsValid()) return user.user_id;
  }
  return json_string(body, {"userid", "user_id"}).value_or("demo-user");
}

std::string request_user_id_from_crow(const crow::request& req,
                                      const std::shared_ptr<AuthMiddleware>& auth_middleware) {
  if (auth_middleware != nullptr) {
    auto user = auth_middleware->ExtractUser(req);
    if (user.IsValid()) return user.user_id;
  }
  return "demo-user";
}

bool has_replay_auth_header(const crow::request& req) {
  return !req.get_header_value("X-User-Id").empty() ||
         !req.get_header_value("Authorization").empty();
}

std::optional<GatewayUserContext> replay_authenticated_user(
    const crow::request& req,
    const std::shared_ptr<AuthMiddleware>& auth_middleware,
    crow::response* error_response) {
  if (auth_middleware == nullptr) return std::nullopt;
  if (!has_replay_auth_header(req)) {
    if (error_response != nullptr) {
      *error_response = json_error(401, "unauthenticated",
                                   "Replay endpoint requires authentication");
    }
    return std::nullopt;
  }
  auto user = auth_middleware->ExtractUser(req);
  if (!user.IsValid()) {
    if (error_response != nullptr) {
      *error_response = json_error(401, "unauthenticated",
                                   "Replay endpoint requires a valid user");
    }
    return std::nullopt;
  }
  return user;
}

std::optional<std::string> replay_session_owner(const crow::json::wvalue& session) {
  const auto parsed = json::parse(session.dump(), nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
  return json_string(parsed, {"userid", "user_id", "userId"});
}

bool can_read_replay_session(const GatewayUserContext& user,
                             const std::string& session_id,
                             const crow::json::wvalue& session,
                             const std::shared_ptr<cex::backtest::app::RbacEngine>& rbac_engine,
                             std::string* error) {
  if (rbac_engine != nullptr) {
    auto auth = rbac_engine->Authorize(user.user_id, "read", "replay_session", session_id);
    if (!auth.allowed) {
      if (error != nullptr) *error = auth.reason.value_or("Forbidden");
      return false;
    }
    const auto owner = replay_session_owner(session);
    if (owner.has_value() &&
        !rbac_engine->CanAccessSession(user.user_id, session_id, *owner)) {
      if (error != nullptr) *error = "Replay session is not owned by requester";
      return false;
    }
  }
  return true;
}

}  // namespace

// F-05: контекст подписки на market data для каждого WS-соединения.
// Вынесен из run() в namespace-уровень — Crow требует copyable/movable типы в хендлерах.
struct MarketDataWsState {
  std::unique_ptr<grpc::ClientContext> grpc_ctx;
  std::atomic<bool> streaming{false};
  std::string current_asset;
  std::mutex mu;
};

static fob::common::v1::Decimal dec_from_double(double x, int32_t scale) {
  double factor = std::pow(10.0, static_cast<double>(scale));
  int64_t units = static_cast<int64_t>(std::llround(x * factor));
  fob::common::v1::Decimal d;
  d.set_units(units);
  d.set_scale(scale);
  return d;
}

static bool has(const crow::json::rvalue& v, const char* key) {
  return v.has(key) && v[key].t() != crow::json::type::Null;
}

static std::string get_string(const crow::json::rvalue& v, const char* key, const std::string& def = "") {
  if (!has(v, key)) return def;
  if (v[key].t() == crow::json::type::String) return std::string(v[key].s());
  std::ostringstream oss;
  oss << v[key].d();
  return oss.str();
}

static double get_double(const crow::json::rvalue& v, const char* key) {
  if (!has(v, key)) return 0.0;
  if (v[key].t() == crow::json::type::Number) return v[key].d();
  if (v[key].t() == crow::json::type::String) return std::stod(std::string(v[key].s()));
  return 0.0;
}

static fob::common::v1::Side parse_side(const std::string& s) {
  std::string x = s;
  for (auto& c : x) c = static_cast<char>(std::tolower(c));
  if (x == "buy" || x == "b") return fob::common::v1::SIDE_BUY;
  if (x == "sell" || x == "s") return fob::common::v1::SIDE_SELL;
  return fob::common::v1::SIDE_UNSPECIFIED;
}

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

static std::string decimal_to_json(const fob::common::v1::Decimal& d) {
  double value = static_cast<double>(d.units()) / std::pow(10.0, d.scale());
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(8) << value;
  return oss.str();
}

static double decimal_to_double(const fob::common::v1::Decimal& d) {
  return static_cast<double>(d.units()) / std::pow(10.0, d.scale());
}

static std::string side_to_string(const fob::common::v1::Side side) {
  switch (side) {
    case fob::common::v1::SIDE_BUY:
      return "buy";
    case fob::common::v1::SIDE_SELL:
      return "sell";
    default:
      return "unspecified";
  }
}

static std::string order_status_to_string(const fob::common::v1::OrderStatus status) {
  switch (status) {
    case fob::common::v1::ORDER_STATUS_NEW:
      return "new";
    case fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED:
      return "partial";
    case fob::common::v1::ORDER_STATUS_FILLED:
      return "filled";
    case fob::common::v1::ORDER_STATUS_CANCELED:
      return "cancelled";
    case fob::common::v1::ORDER_STATUS_REJECTED:
      return "rejected";
    case fob::common::v1::ORDER_STATUS_EXPIRED:
      return "expired";
    default:
      return "unspecified";
  }
}

static int64_t timestamp_to_unix_ms(const google::protobuf::Timestamp& ts) {
  return ts.seconds() * 1000 + ts.nanos() / 1000000;
}

static uint64_t get_uint64_param(const crow::request& req,
                                 const char* key,
                                 uint64_t def = 0) {
  const char* raw = req.url_params.get(key);
  if (raw == nullptr) return def;
  try {
    return static_cast<uint64_t>(std::stoull(raw));
  } catch (...) {
    return def;
  }
}

struct ReplayWsContext {
  std::string session_id;
  uint64_t after_sequence{0};
  uint64_t listener_id{0};
  cex::gateway::infra::ReplayResultsHub* hub{nullptr};
  std::mutex mu;
  bool closed{false};
};

void close_replay_ws_context(crow::websocket::connection& conn) {
  auto* holder = static_cast<std::shared_ptr<ReplayWsContext>*>(conn.userdata());
  if (holder == nullptr) return;
  if (*holder) {
    {
      std::lock_guard<std::mutex> lock((*holder)->mu);
      (*holder)->closed = true;
    }
    if ((*holder)->hub != nullptr && (*holder)->listener_id != 0) {
      (*holder)->hub->Unsubscribe((*holder)->listener_id);
      (*holder)->listener_id = 0;
    }
  }
  delete holder;
  conn.userdata(nullptr);
}

static crow::json::wvalue flow_order_to_json(const fob::orders::v1::FlowOrder& order) {
  crow::json::wvalue out;
  out["order_id"] = order.order_id();
  out["client_order_id"] = order.client_order_id();
  out["user_id"] = order.user_id();
  out["account_id"] = order.account_id();
  out["symbol"] = order.instrument().symbol();
  out["base"] = order.instrument().base();
  out["quote"] = order.instrument().quote();
  out["side"] = side_to_string(order.side());
  out["total_qty"] = decimal_to_double(order.total_qty());
  out["remaining_qty"] = decimal_to_double(order.remaining_qty());
  out["price_low"] = decimal_to_double(order.price_low());
  out["price_high"] = decimal_to_double(order.price_high());
  out["max_speed"] = decimal_to_double(order.max_speed());
  out["status"] = order_status_to_string(order.status());
  out["created_at_ms"] = order.has_created_at() ? timestamp_to_unix_ms(order.created_at()) : 0;
  out["updated_at_ms"] = order.has_updated_at() ? timestamp_to_unix_ms(order.updated_at()) : 0;
  return out;
}

HttpGateway::HttpGateway(infra::OrderFlowClient order_flow_client,
                         infra::MarketDataClient market_data_client,
                         std::shared_ptr<SummaryRepository> summary_repo,
                         std::shared_ptr<LogsRepository> logs_repo,
                         std::shared_ptr<SessionsRepository> sessions_repo,
                         std::shared_ptr<CompareRepository> compare_repo,
                         std::shared_ptr<cex::backtest::app::RbacEngine> rbac_engine,
                         infra::ReplayCommandsKafkaPublisher* replay_commands,
                         infra::ReplayResultsHub* replay_results,
                         std::shared_ptr<AuthMiddleware> auth_middleware)
    : order_flow_(std::move(order_flow_client)),
      market_data_(std::move(market_data_client)),
      summary_repo_(std::move(summary_repo)),
      logs_repo_(std::move(logs_repo)),
      sessions_repo_(std::move(sessions_repo)),
      compare_repo_(std::move(compare_repo)),
      rbac_engine_(std::move(rbac_engine)),
      replay_commands_(replay_commands),
      replay_results_(replay_results),
      auth_middleware_(std::move(auth_middleware)) {}

struct CorsMiddleware {
  struct context {};
  void before_handle(crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/) {}
  void after_handle(crow::request& /*req*/, crow::response& res, context& /*ctx*/) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, api_key");
  }
};

void HttpGateway::run(uint16_t port) {
  crow::App<CorsMiddleware> app;

  // Liveness/readiness probes.
  CROW_ROUTE(app, "/healthz")([] { return crow::response(200, "ok"); });
  CROW_ROUTE(app, "/readyz")([] { return crow::response(200, "ready"); });

  // Create flow order
  CROW_ROUTE(app, "/v1/flow-orders").methods(crow::HTTPMethod::Post)(
      [this](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) {
          return crow::response(400, "Invalid JSON");
        }

        fob::orders::v1::FlowOrder order;
        order.set_order_id(cex::common::uuid_v4());
        order.set_client_order_id(get_string(body, "client_order_id", ""));
        order.set_user_id(get_string(body, "user_id", "demo-user"));
        order.set_account_id(get_string(body, "account_id", "demo-account"));

        std::string symbol = get_string(body, "symbol", "BTC/USDT");
        *order.mutable_instrument() = parse_instrument(symbol);
        order.set_side(parse_side(get_string(body, "side", "buy")));

        *order.mutable_total_qty() = dec_from_double(get_double(body, "total_qty"), 8);
        *order.mutable_remaining_qty() = order.total_qty();
        *order.mutable_price_low() = dec_from_double(get_double(body, "price_low"), 2);
        *order.mutable_price_high() = dec_from_double(get_double(body, "price_high"), 2);
        *order.mutable_max_speed() = dec_from_double(get_double(body, "max_speed"), 8);

        order.set_status(fob::common::v1::ORDER_STATUS_NEW);
        *order.mutable_created_at() = cex::common::now_ts();
        *order.mutable_updated_at() = cex::common::now_ts();

        fob::orders::v1::CreateFlowOrderRequest grpc_req;
        auto* meta = grpc_req.mutable_meta();
        meta->set_event_id(cex::common::uuid_v4());
        *meta->mutable_ts_event() = cex::common::now_ts();
        meta->set_source("gateway");
        meta->set_correlation_id(cex::common::uuid_v4());
        meta->set_partition_key(order.user_id());
        *grpc_req.mutable_order() = order;

        auto grpc_resp = order_flow_.CreateFlowOrder(grpc_req);

        crow::json::wvalue out;
        out["accepted"] = grpc_resp.accepted();
        out["order_id"] = grpc_resp.order_id();
        if (grpc_resp.has_error()) {
          out["error"]["code"] = grpc_resp.error().code();
          out["error"]["message"] = grpc_resp.error().message();
        }
        return crow::response(200, out);
      });

  CROW_ROUTE(app, "/v1/flow-orders").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req) {
        fob::orders::v1::ListFlowOrdersRequest grpc_req;
        auto* meta = grpc_req.mutable_meta();
        meta->set_event_id(cex::common::uuid_v4());
        *meta->mutable_ts_event() = cex::common::now_ts();
        meta->set_source("gateway");
        meta->set_correlation_id(cex::common::uuid_v4());
        const char* user_id = req.url_params.get("user_id");
        grpc_req.set_user_id(user_id ? user_id : "demo-user");
        meta->set_partition_key(grpc_req.user_id());

        const auto grpc_resp = order_flow_.ListFlowOrders(grpc_req);
        crow::json::wvalue out;
        for (int index = 0; index < grpc_resp.views_size(); ++index) {
          out["orders"][index] = flow_order_to_json(grpc_resp.views(index).order());
          if (grpc_resp.views(index).has_error() &&
              !grpc_resp.views(index).error().code().empty()) {
            out["orders"][index]["error"]["code"] = grpc_resp.views(index).error().code();
            out["orders"][index]["error"]["message"] = grpc_resp.views(index).error().message();
          }
        }
        return crow::response(200, out);
      });

  CROW_ROUTE(app, "/v1/flow-orders/<string>").methods(crow::HTTPMethod::Get)(
      [this](const crow::request&, const std::string& order_id) {
        fob::orders::v1::GetFlowOrderRequest grpc_req;
        auto* meta = grpc_req.mutable_meta();
        meta->set_event_id(cex::common::uuid_v4());
        *meta->mutable_ts_event() = cex::common::now_ts();
        meta->set_source("gateway");
        meta->set_correlation_id(cex::common::uuid_v4());
        meta->set_partition_key(order_id);
        grpc_req.set_user_id("demo-user");
        grpc_req.set_order_id(order_id);

        const auto grpc_resp = order_flow_.GetFlowOrder(grpc_req);
        if (grpc_resp.view().has_error() && grpc_resp.view().error().code() == "NOT_FOUND") {
          crow::json::wvalue out;
          out["error"]["code"] = grpc_resp.view().error().code();
          out["error"]["message"] = grpc_resp.view().error().message();
          return crow::response(404, out);
        }

        crow::json::wvalue out;
        out["order"] = flow_order_to_json(grpc_resp.view().order());
        if (grpc_resp.view().has_error() &&
            !grpc_resp.view().error().code().empty()) {
          out["error"]["code"] = grpc_resp.view().error().code();
          out["error"]["message"] = grpc_resp.view().error().message();
        }
        return crow::response(200, out);
      });

  CROW_ROUTE(app, "/v1/flow-orders/<string>").methods(crow::HTTPMethod::Delete)(
      [this](const crow::request& req, const std::string& order_id) {
        crow::json::rvalue body;
        if (!req.body.empty()) {
          body = crow::json::load(req.body);
          if (!body) {
            return crow::response(400, "Invalid JSON");
          }
        }

        fob::orders::v1::CancelFlowOrderRequest grpc_req;
        auto* meta = grpc_req.mutable_meta();
        meta->set_event_id(cex::common::uuid_v4());
        *meta->mutable_ts_event() = cex::common::now_ts();
        meta->set_source("gateway");
        meta->set_correlation_id(cex::common::uuid_v4());
        meta->set_partition_key(order_id);
        grpc_req.set_user_id(body ? get_string(body, "user_id", "demo-user") : "demo-user");
        grpc_req.set_order_id(order_id);
        grpc_req.set_reason(body ? get_string(body, "reason", "cancelled_from_gateway")
                                 : "cancelled_from_gateway");

        const auto grpc_resp = order_flow_.CancelFlowOrder(grpc_req);
        crow::json::wvalue out;
        out["success"] = grpc_resp.success();
        if (grpc_resp.has_error() && !grpc_resp.error().code().empty()) {
          out["error"]["code"] = grpc_resp.error().code();
          out["error"]["message"] = grpc_resp.error().message();
        }
        if (!grpc_resp.success() && grpc_resp.has_error() &&
            grpc_resp.error().code() == "NOT_FOUND") {
          return crow::response(404, out);
        }
        return crow::response(grpc_resp.success() ? 200 : 400, out);
      });

  CROW_ROUTE(app, "/api/v1/stream/bbo").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req) {
        (void)req;
        
        crow::response res;
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Credentials", "true");

        auto stream = market_data_.SubscribeBBO();
        if (!stream) {
          cex::common::log_json("ERROR", "Failed to subscribe to BBO stream");
          return crow::response(500, "Failed to subscribe to BBO stream");
        }

        fob::marketdata::v1::BBOUpdate update;
        while (stream->Read(&update)) {
          std::ostringstream json;
          json << std::fixed << std::setprecision(8);
          json << "{";
          json << "\"venue\":\"" << update.venue() << "\",";
          json << "\"symbol\":\"" << update.instrument().symbol() << "\",";
          json << "\"bid_price\":" << decimal_to_json(update.bid_price()) << ",";
          json << "\"bid_volume\":" << decimal_to_json(update.bid_volume()) << ",";
          json << "\"ask_price\":" << decimal_to_json(update.ask_price()) << ",";
          json << "\"ask_volume\":" << decimal_to_json(update.ask_volume()) << ",";
          json << "\"mid_price\":" << decimal_to_json(update.mid_price()) << ",";
          json << "\"spread_absolute\":" << decimal_to_json(update.spread_absolute()) << ",";
          json << "\"spread_relative_percent\":" << update.spread_relative_percent() << ",";
          json << "\"microprice\":" << decimal_to_json(update.microprice());
          json << "}";

          res.write("data: " + json.str() + "\n\n");
        }

        return res;
      });

  CROW_ROUTE(app, "/api/v1/replay/commands").methods(crow::HTTPMethod::Post)(
      [this](const crow::request& req) {
        // AUTH CHECK
        if (auth_middleware_) {
          auto user = auth_middleware_->ExtractUser(req);
          std::string error;
          if (!auth_middleware_->Authorize(user, "create", "replay_session", "", &error)) {
            crow::json::wvalue err;
            err["error"] = "Forbidden";
            err["reason"] = error;
            return crow::response(403, err);
          }
        }

        if (replay_commands_ == nullptr) {
          return crow::response(503, "Replay commands publisher unavailable");
        }
        auto body = crow::json::load(req.body);
        if (!body) {
          return crow::response(400, "Invalid JSON");
        }

        cex::common::ReplayCommandMessage msg;
        msg.command_id = cex::common::uuid_v4();
        msg.session_id = get_string(body, "session_id", "");
        msg.command_type = cex::common::ReplayCommandTypeFromString(
            get_string(body, "type", "unknown"));
        msg.requested_by = get_string(body, "requested_by", "ui");
        msg.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        msg.payload_json = req.body;

        if (msg.session_id.empty() || msg.command_type == cex::common::ReplayCommandType::kUnknown) {
          return crow::response(400, "session_id and valid type are required");
        }

        const bool ok = replay_commands_->Publish(msg);
        crow::json::wvalue out;
        out["accepted"] = ok;
        out["command_id"] = msg.command_id;
        out["session_id"] = msg.session_id;
        out["type"] = cex::common::ReplayCommandTypeToString(msg.command_type);
        return crow::response(ok ? 202 : 502, out);
      });

  CROW_ROUTE(app, "/api/v1/replay/results/latest").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req) {
        if (replay_results_ == nullptr) {
          return crow::response(503, "Replay results hub unavailable");
        }
        const char* session_id = req.url_params.get("session_id");
        if (session_id == nullptr) session_id = req.url_params.get("sessionid");
        if (session_id == nullptr || std::string(session_id).empty()) {
          return crow::response(400, "session_id is required");
        }
        const auto latest = replay_results_->Latest(session_id);
        if (!latest.has_value()) {
          return crow::response(404, "Replay result not found");
        }
        return crow::response(200, cex::common::ReplayResultMessageToJson(*latest));
      });

  CROW_WEBSOCKET_ROUTE(app, "/api/v1/replay/results/ws")
      .onaccept([this](const crow::request& req, void** userdata) {
        if (replay_results_ == nullptr) return false;
        auto ctx = std::make_shared<ReplayWsContext>();
        const char* session_id_raw = req.url_params.get("session_id");
        if (session_id_raw == nullptr) session_id_raw = req.url_params.get("sessionid");
        ctx->session_id = session_id_raw == nullptr ? "" : session_id_raw;
        ctx->after_sequence = get_uint64_param(req, "after_seq", 0);
        ctx->hub = replay_results_;
        *userdata = new std::shared_ptr<ReplayWsContext>(std::move(ctx));
        return true;
      })
      .onopen([](crow::websocket::connection& conn) {
        auto* holder = static_cast<std::shared_ptr<ReplayWsContext>*>(conn.userdata());
        if (holder == nullptr || !*holder || (*holder)->hub == nullptr) {
          conn.close("Replay results hub unavailable");
          return;
        }
        auto ctx = *holder;
        ctx->listener_id = ctx->hub->Subscribe(
            [ctx, &conn](const cex::gateway::infra::ReplayResultsHub::EventEnvelope& envelope) {
              if (!ctx->session_id.empty() &&
                  envelope.message.session_id != ctx->session_id) {
                return;
              }
              std::lock_guard<std::mutex> lock(ctx->mu);
              if (ctx->closed || envelope.sequence <= ctx->after_sequence) return;
              ctx->after_sequence = envelope.sequence;
              conn.send_text(cex::common::ReplayResultMessageToJson(envelope.message));
            });
      })
      .onmessage([](crow::websocket::connection&,
                    const std::string&,
                    bool) {
        // Replay result WebSocket is server-push only. Client messages are ignored.
      })
      .onclose([](crow::websocket::connection& conn,
                  const std::string&,
                  uint16_t) {
        close_replay_ws_context(conn);
      })
      .onerror([](crow::websocket::connection& conn, const std::string&) {
        close_replay_ws_context(conn);
      });

  CROW_ROUTE(app, "/api/v1/replay/results/stream").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req) {
        if (replay_results_ == nullptr) {
          return crow::response(503, "Replay results hub unavailable");
        }

        // T-F15-007: bound concurrent SSE streams so they cannot exhaust the
        // Crow worker pool and starve REST endpoints. Capacity is kept below
        // the pool size (REPLAY_GW_THREADS) so REST always has headroom.
        static std::atomic<int> active_streams{0};
        const int max_streams =
            cex::common::Env::get_int("REPLAY_SSE_MAX_STREAMS", 32);
        if (active_streams.fetch_add(1) + 1 > max_streams) {
          active_streams.fetch_sub(1);
          crow::response busy(503, "Too many active replay streams");
          busy.set_header("Retry-After", "5");
          return busy;
        }
        struct ActiveStreamGuard {
          std::atomic<int>& counter;
          ~ActiveStreamGuard() { counter.fetch_sub(1); }
        } active_stream_guard{active_streams};

        const char* session_id_raw = req.url_params.get("session_id");
        if (session_id_raw == nullptr) session_id_raw = req.url_params.get("sessionid");
        const std::string session_id = session_id_raw == nullptr ? "" : session_id_raw;
        uint64_t after_sequence = get_uint64_param(req, "after_seq", 0);
        // On native EventSource reconnect the cursor arrives as the
        // Last-Event-ID header, not the query param — honour it so the client
        // resumes instead of replaying from sequence 0.
        const std::string last_event_id = req.get_header_value("Last-Event-ID");
        if (!last_event_id.empty()) {
          try {
            after_sequence =
                std::max<uint64_t>(after_sequence, std::stoull(last_event_id));
          } catch (...) {
          }
        }

        crow::response res;
        res.code = 200;
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Credentials", "true");

        // Bounded long-poll: hold the connection at most REPLAY_SSE_MAX_DURATION_S
        // and break immediately if the client disconnects (res.is_alive() reads
        // the socket state). The browser EventSource reconnects automatically,
        // so this delivers events with low latency while guaranteeing the worker
        // thread is released — abandoned streams no longer pin a thread forever.
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(
                cex::common::Env::get_int("REPLAY_SSE_MAX_DURATION_S", 30));
        bool wrote_event = false;
        while (std::chrono::steady_clock::now() < deadline && res.is_alive()) {
          infra::ReplayResultsHub::EventEnvelope envelope;
          if (!replay_results_->WaitNext(session_id, after_sequence,
                                         std::chrono::milliseconds(2000), &envelope)) {
            if (wrote_event) break;  // delivered a batch already → return now
            res.write(": keepalive\n\n");
            continue;
          }
          after_sequence = envelope.sequence;
          res.write("id: " + std::to_string(envelope.sequence) + "\n");
          res.write("event: replay\n");
          res.write("data: " + cex::common::ReplayResultMessageToJson(envelope.message) + "\n\n");
          wrote_event = true;
        }
        return res;
      });

  CROW_ROUTE(app, "/api/v1/replay/sessions").methods(crow::HTTPMethod::Post)(
      [this](const crow::request& req) {
        if (replay_commands_ == nullptr) {
          return json_error(503, "dependency_error",
                            "Replay commands publisher unavailable");
        }

        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
          return json_error(400, "invalid_json", "Invalid JSON");
        }

        if (auth_middleware_) {
          auto user = auth_middleware_->ExtractUser(req);
          std::string error;
          if (!auth_middleware_->Authorize(user, "create", "replay_session", "", &error)) {
            return json_error(403, "permission_denied", error);
          }
        }

        std::string validation_error;
        if (!validate_replay_create_payload(body, &validation_error)) {
          return json_error(400, "validation_error", validation_error);
        }

        const std::string session_id = cex::common::uuid_v4();
        const std::string user_id = request_user_id(req, auth_middleware_, body);
        const int64_t created_at_ms = now_millis();

        body["sessionid"] = session_id;
        body["userid"] = user_id;

        cex::common::ReplayCommandMessage msg;
        msg.command_id = cex::common::uuid_v4();
        msg.session_id = session_id;
        msg.command_type = cex::common::ReplayCommandType::kStart;
        msg.requested_by = user_id;
        msg.created_at_ms = created_at_ms;
        msg.payload_json = body.dump();

        const bool ok = replay_commands_->Publish(msg);
        if (!ok) {
          return json_error(502, "command_publish_failed",
                            "Failed to publish replay start command");
        }

        crow::json::wvalue out;
        out["accepted"] = true;
        out["command_id"] = msg.command_id;
        out["sessionid"] = session_id;
        out["userid"] = user_id;
        out["commandstatus"] = "accepted";
	        if (sessions_repo_ != nullptr) {
	          const auto deadline = std::chrono::steady_clock::now() +
	                                std::chrono::seconds(2);
	          while (std::chrono::steady_clock::now() < deadline) {
	            auto created = sessions_repo_->GetSession(session_id);
	            if (created.has_value()) {
	              (*created)["accepted"] = true;
	              (*created)["command_id"] = msg.command_id;
	              return crow::response(202, *created);
	            }
	            std::this_thread::sleep_for(std::chrono::milliseconds(50));
	          }
	          cex::common::log_json("WARN",
	                                "Replay session was not visible before create response",
	                                {{"session_id", session_id}});
	        }
	        return crow::response(202, out);
	      });

  CROW_ROUTE(app, "/api/v1/replay/sessions").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req) {
        if (sessions_repo_ == nullptr) {
          return crow::response(503, "Replay session storage unavailable");
        }
        std::optional<std::string> owner_filter;
        if (auth_middleware_ != nullptr) {
          crow::response auth_error;
          auto user = replay_authenticated_user(req, auth_middleware_, &auth_error);
          if (!user.has_value()) return auth_error;
          std::string error;
          if (!auth_middleware_->Authorize(*user, "read", "replay_session", "", &error)) {
            return json_error(403, "permission_denied", error);
          }
          owner_filter = user->user_id;
        }
        return crow::response(200, sessions_repo_->ListSessions(req, owner_filter));
      });

  CROW_ROUTE(app, "/api/v1/replay/sessions/<string>").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req, const std::string& session_id) {
        if (session_id.empty()) {
          return crow::response(400, "session_id is required");
        }
        if (sessions_repo_ == nullptr) {
          return crow::response(503, "Replay session storage unavailable");
        }
        auto session = sessions_repo_->GetSession(session_id);
        if (!session.has_value()) {
          return crow::response(404, "Replay session not found");
        }
        if (auth_middleware_ != nullptr) {
          crow::response auth_error;
          auto user = replay_authenticated_user(req, auth_middleware_, &auth_error);
          if (!user.has_value()) return auth_error;
          std::string error;
          if (!can_read_replay_session(*user, session_id, *session, rbac_engine_, &error)) {
            return json_error(403, "permission_denied", error);
          }
        }
        return crow::response(200, *session);
      });

  CROW_ROUTE(app, "/api/v1/replay/compare").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req) {
        const char* session_a = req.url_params.get("sessionA");
        const char* session_b = req.url_params.get("sessionB");
        if (session_a == nullptr || session_b == nullptr ||
            std::string(session_a).empty() || std::string(session_b).empty()) {
          return crow::response(400, "sessionA and sessionB are required");
        }
        if (compare_repo_ == nullptr) {
          return crow::response(503, "Replay compare storage unavailable");
        }
        if (auth_middleware_ != nullptr) {
          if (sessions_repo_ == nullptr) {
            return crow::response(503, "Replay session storage unavailable");
          }
          crow::response auth_error;
          auto user = replay_authenticated_user(req, auth_middleware_, &auth_error);
          if (!user.has_value()) return auth_error;
          const auto session_a_row = sessions_repo_->GetSession(session_a);
          const auto session_b_row = sessions_repo_->GetSession(session_b);
          if (!session_a_row.has_value() || !session_b_row.has_value()) {
            return crow::response(404, "Replay session not found");
          }
          std::string error;
          if (!can_read_replay_session(*user, session_a, *session_a_row,
                                       rbac_engine_, &error) ||
              !can_read_replay_session(*user, session_b, *session_b_row,
                                       rbac_engine_, &error)) {
            return json_error(403, "permission_denied", error);
          }
        }
        return crow::response(200, compare_repo_->Compare(session_a, session_b));
      });

  CROW_ROUTE(app, "/api/v1/replay/audit").methods(crow::HTTPMethod::Get)(
      [](const crow::request&) {
        return json_error(
            501,
            "audit_mode_disabled",
            "Replay audit mode is feature-flagged off in Gateway");
      });

  CROW_ROUTE(app, "/api/v1/replay/sessions/<string>/retry").methods(crow::HTTPMethod::Post)(
      [this](const crow::request& req, const std::string& session_id) {
        if (session_id.empty()) {
          return json_error(400, "validation_error", "session_id is required");
        }
        if (replay_commands_ == nullptr) {
          return json_error(503, "dependency_error",
                            "Replay commands publisher unavailable");
        }

        json body = json::object();
        if (!req.body.empty()) {
          body = json::parse(req.body, nullptr, false);
          if (body.is_discarded() || !body.is_object()) {
            return json_error(400, "invalid_json", "Invalid JSON");
          }
        }

        if (auth_middleware_) {
          auto user = auth_middleware_->ExtractUser(req);
          std::string error;
          if (!auth_middleware_->Authorize(user, "retry", "replay_session", session_id, &error)) {
            return json_error(403, "permission_denied", error);
          }
        }

        const std::string user_id = request_user_id(req, auth_middleware_, body);
        const std::string retry_session_id = cex::common::uuid_v4();
        body["originalsessionid"] = session_id;
        body["newsessionid"] = retry_session_id;
        body["userid"] = user_id;

        cex::common::ReplayCommandMessage msg;
        msg.command_id = cex::common::uuid_v4();
        msg.session_id = session_id;
        msg.command_type = cex::common::ReplayCommandType::kRetry;
        msg.requested_by = user_id;
        msg.created_at_ms = now_millis();
        msg.payload_json = body.dump();

        const bool ok = replay_commands_->Publish(msg);
        if (!ok) {
          return json_error(502, "command_publish_failed",
                            "Failed to publish replay retry command");
        }

        crow::json::wvalue out;
        out["accepted"] = true;
        out["command_id"] = msg.command_id;
        out["sessionid"] = retry_session_id;
        out["retryparentid"] = session_id;
        out["userid"] = user_id;
        out["status"] = "pending";
        out["progressbatches"] = 0;
        out["totalbatches"] = 0;
        return crow::response(202, out);
      });

  CROW_ROUTE(app, "/api/v1/replay/sessions/<string>").methods(crow::HTTPMethod::Delete)(
      [this](const crow::request& req, const std::string& session_id) {
        if (session_id.empty()) {
          return crow::response(400, "session_id is required");
        }
        if (replay_commands_ == nullptr) {
          return crow::response(503, "Replay commands publisher unavailable");
        }

        std::string user_id = "demo-user";
        if (auth_middleware_) {
          auto user = auth_middleware_->ExtractUser(req);
          if (user.IsValid()) user_id = user.user_id;
          std::string error;
          if (!auth_middleware_->Authorize(user, "cancel", "replay_session", session_id, &error)) {
            return json_error(403, "permission_denied", error);
          }
        }

        std::string payload_json;
        if (!req.body.empty()) {
          auto body = crow::json::load(req.body);
          if (!body) {
            return crow::response(400, "Invalid JSON");
          }
          payload_json = req.body;
        }

        cex::common::ReplayCommandMessage msg;
        msg.command_id   = cex::common::uuid_v4();
        msg.session_id   = session_id;
        msg.command_type = cex::common::ReplayCommandType::kCancel;
        msg.requested_by = user_id;
        msg.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        msg.payload_json = payload_json;

        const bool ok = replay_commands_->Publish(msg);
        if (!ok) {
          return json_error(502, "command_publish_failed",
                            "Failed to publish replay cancel command");
        }
        crow::json::wvalue out;
        out["accepted"]   = true;
        out["command_id"] = msg.command_id;
        out["sessionid"] = session_id;
        out["userid"] = user_id;
        out["commandstatus"] = "accepted";
        if (sessions_repo_ != nullptr) {
          auto current = sessions_repo_->GetSession(session_id);
          if (current.has_value()) {
            (*current)["accepted"] = true;
            (*current)["command_id"] = msg.command_id;
            return crow::response(202, *current);
          }
        }
        return crow::response(202, out);
      });

  // --- [F15-API-4] GET /api/v1/replay/sessions/{id}/summary ---
  CROW_ROUTE(app, "/api/v1/replay/sessions/<string>/summary").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req, const std::string& session_id) {
        if (session_id.empty()) {
          return crow::response(400, "session_id is required");
        }
        
        const std::string user_id = request_user_id_from_crow(req, auth_middleware_);
        if (rbac_engine_ != nullptr) {
          auto auth = rbac_engine_->Authorize(user_id, "read", "replay_session", session_id);
          if (!auth.allowed) {
            return crow::response(403, auth.reason.value_or("Forbidden"));
          }
        }

        // 2. Сначала проверяем Hub (Real-time данные)
        if (replay_results_) {
            auto latest = replay_results_->Latest(session_id);
            if (latest.has_value() && latest->has_summary) {
                // Return a ReplaySummary-shaped payload even when the latest
                // value comes from Kafka/WebSocket memory, not PostgreSQL.
                return crow::response(200, replay_result_summary_json(*latest));
            }
        }

        // 3. Если в Hub нет, идем в PostgreSQL
        if (!summary_repo_) return crow::response(503, "Summary storage unavailable");
        auto summary = summary_repo_->GetSummary(session_id);
        return crow::response(200, summary);
      });

  // --- [F15-API-4] GET /api/v1/replay/sessions/{id}/agentlogs ---
  CROW_ROUTE(app, "/api/v1/replay/sessions/<string>/agentlogs").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req, const std::string& session_id) {
        if (session_id.empty()) {
          return crow::response(400, "session_id is required");
        }

        const std::string user_id = request_user_id_from_crow(req, auth_middleware_);
        if (rbac_engine_ != nullptr &&
            !rbac_engine_->Authorize(user_id, "read", "replay_session", session_id).allowed) {
            return crow::response(403, "Permission denied");
        }

        // 2. Извлекаем параметры пагинации и фильтрации
        int limit = static_cast<int>(get_uint64_param(req, "limit", 100));
        int offset = static_cast<int>(get_uint64_param(req, "offset", 0));
        std::string filter = req.url_params.get("filter") ? req.url_params.get("filter") : "";
        const char* filter_keys[] = {"batchseq",
                                     "pnl",
                                     "isvalue",
                                     "is_value",
                                     "fillrate",
                                     "fill_rate",
                                     "solver_error",
                                     "solvererror",
                                     "solver_error_flag"};
        for (const char* key : filter_keys) {
          const char* value = req.url_params.get(key);
          if (value != nullptr && std::string(value).empty() == false) {
            if (!filter.empty()) filter += ";";
            filter += std::string(key) + "=" + value;
          }
        }

        // 3. Запрос в ClickHouse
        if (!logs_repo_) return crow::response(503, "Logs storage unavailable");
        auto logs = logs_repo_->GetAgentLogs(session_id, limit, offset, filter);
        
        return crow::response(200, logs);
      });

  CROW_ROUTE(app, "/api/v1/hedge-pnl").methods(crow::HTTPMethod::Get)(
    [this](const crow::request& req) {
        const char* batch_id = req.url_params.get("batch_id");
        const char* session_id = req.url_params.get("session_id");
        const char* user_id = req.url_params.get("user_id");
        
        if ((!batch_id || std::string(batch_id).empty()) &&
            (!session_id || std::string(session_id).empty()) &&
            (!user_id || std::string(user_id).empty())) {
            crow::json::wvalue err;
            err["error"] = "Bad Request";
            err["message"] = "At least one of batch_id, session_id, or user_id is required";
            return crow::response(400, err);
        }
        
        if (auth_middleware_) {
            auto user = auth_middleware_->ExtractUser(req);
            std::string error;
            if (!auth_middleware_->Authorize(user, "read", "hedge_pnl", 
                                            user_id ? user_id : "", &error)) {
                crow::json::wvalue err;
                err["error"] = "Forbidden";
                err["reason"] = error;
                return crow::response(403, err);
            }
        }
        
        fob::marketdata::v1::GetHedgePnLRequest grpc_req;
        if (batch_id && std::string(batch_id).length() > 0) 
            grpc_req.set_batch_id(batch_id);
        if (session_id && std::string(session_id).length() > 0) 
            grpc_req.set_session_id(session_id);
        if (user_id && std::string(user_id).length() > 0) 
            grpc_req.set_user_id(user_id);
        
        const char* from_time = req.url_params.get("from_time");
        const char* to_time = req.url_params.get("to_time");
        if (from_time) {
            auto* ts = grpc_req.mutable_from_time();
            ts->set_seconds(std::stoll(from_time));
        }
        if (to_time) {
            auto* ts = grpc_req.mutable_to_time();
            ts->set_seconds(std::stoll(to_time));
        }
        
        auto grpc_resp = market_data_.GetHedgePnL(grpc_req);
        
        crow::json::wvalue out;
        out["total_records"] = grpc_resp.records_size();
        
        for (int i = 0; i < grpc_resp.records_size(); ++i) {
            const auto& rec = grpc_resp.records(i);
            auto& record = out["records"][i];
            record["batch_id"] = rec.batch_id();
            record["session_id"] = rec.session_id();
            record["user_id"] = rec.user_id();
            record["venue"] = rec.venue();
            record["instrument"] = rec.instrument();
            record["total_hedge_pnl"] = rec.total_hedge_pnl();
            record["total_filled_qty"] = rec.total_filled_qty();
            record["trade_count"] = rec.trade_count();
            
            if (rec.has_batch_time()) {
                record["batch_time"] = rec.batch_time().seconds();
            }
        }
        
        return crow::response(200, out);
    });

  // ─── F-05 Live Market Data WebSocket ─────────────────────────────────────────
  // Spec §4.5 WebSocket API:
  //   Client connects to ws://host/api/v1/market (без asset в URL)
  //   Client sends: {"action":"subscribe","asset":"BTCUSDT"}
  //   Server sends: {"type":"snapshot","data":{...}} then {"type":"update","data":{...}}
  //   Client sends: {"action":"unsubscribe","asset":"BTCUSDT"}
  //
  // Реализация: один gRPC stream per подписка, MVP N:N mapping.
  // MarketDataWsState определён на namespace-уровне выше.

  CROW_WEBSOCKET_ROUTE(app, "/api/v1/market")
      .onaccept([](const crow::request&, void** userdata) -> bool {
        *userdata = new MarketDataWsState();
        return true;
      })
      .onopen([](crow::websocket::connection& conn) {
        // Приветствие — клиент должен прислать subscribe
        conn.send_text("{\"type\":\"connected\",\"msg\":\"send subscribe action\"}");
      })
      .onmessage([this](crow::websocket::connection& conn,
                        const std::string& msg, bool /*is_binary*/) {
        auto* state = static_cast<MarketDataWsState*>(conn.userdata());
        if (!state) return;

        // Минимальный JSON-парсинг без внешней библиотеки
        auto extract = [&](const std::string& key) -> std::string {
          const std::string needle = "\"" + key + "\":\"";
          auto pos = msg.find(needle);
          if (pos == std::string::npos) return {};
          pos += needle.size();
          auto end = msg.find('"', pos);
          return (end == std::string::npos) ? std::string{} : msg.substr(pos, end - pos);
        };

        const std::string action = extract("action");
        const std::string asset  = extract("asset");

        if (action == "subscribe" && !asset.empty()) {
          // Отменяем предыдущий стрим если был
          {
            std::lock_guard<std::mutex> lg(state->mu);
            if (state->grpc_ctx) {
              state->grpc_ctx->TryCancel();
            }
            state->grpc_ctx   = std::make_unique<grpc::ClientContext>();
            state->streaming  = true;
            state->current_asset = asset;
          }

          // Стартуем gRPC stream в фоне
          std::thread([this, asset, &conn, state]() mutable {
            grpc::ClientContext* ctx_raw;
            {
              std::lock_guard<std::mutex> lg(state->mu);
              ctx_raw = state->grpc_ctx.get();
            }
            if (!ctx_raw) return;

            auto reader = market_data_.SubscribeMarketData(asset, ctx_raw);
            if (!reader) {
              try { conn.send_text("{\"type\":\"error\",\"msg\":\"stream unavailable\"}"); }
              catch (...) {}
              return;
            }

            fob::marketdata::v1::MarketDataStreamEvent event;
            while (state->streaming.load() && reader->Read(&event)) {
              std::string json;
              if (event.has_snapshot()) {
                const auto& s = event.snapshot();
                auto d2s = [](const fob::common::v1::Decimal& d) -> std::string {
                  double v = d.scale() == 0 ? static_cast<double>(d.units())
                           : static_cast<double>(d.units()) / std::pow(10.0, d.scale());
                  return std::to_string(v);
                };
                auto depth2s = [&d2s](const auto& levels) -> std::string {
                  std::string arr = "[";
                  bool first = true;
                  for (const auto& l : levels) {
                    if (!first) arr += ",";
                    first = false;
                    arr += "{\"price\":" + d2s(l.price()) +
                           ",\"qty\":" + d2s(l.qty()) + "}";
                  }
                  arr += "]";
                  return arr;
                };
                json = "{\"type\":\"snapshot\",\"asset\":\"" + s.asset() +
                       "\",\"mid\":" + d2s(s.mid()) +
                       ",\"best_bid\":" + d2s(s.best_bid()) +
                       ",\"best_ask\":" + d2s(s.best_ask()) +
                       ",\"spread\":" + d2s(s.spread()) +
                       ",\"spread_bps\":" + d2s(s.spread_bps()) +
                       ",\"volume_24h\":" + d2s(s.volume_24h()) +
                       ",\"clear_price\":" + d2s(s.clear_price()) +
                       ",\"executed_rate\":" + d2s(s.executed_rate()) +
                       ",\"bid_depth\":" + depth2s(s.bid_depth()) +
                       ",\"ask_depth\":" + depth2s(s.ask_depth()) +
                       ",\"batch_id\":\"" + s.batch_id() +
                       "\",\"source\":\"" + s.source() +
                       "\",\"stale\":" + (s.stale() ? "true" : "false") + "}";
              } else if (event.has_update()) {
                const auto& u = event.update();
                json = "{\"type\":\"update\",\"asset\":\"" + u.asset() +
                       "\",\"source\":\"" + u.source() + "\"}";
              }
              if (!json.empty()) {
                try { conn.send_text(json); } catch (...) { break; }
              }
            }
          }).detach();

          conn.send_text("{\"type\":\"subscribed\",\"asset\":\"" + asset + "\"}");

        } else if (action == "unsubscribe") {
          std::lock_guard<std::mutex> lg(state->mu);
          state->streaming = false;
          if (state->grpc_ctx) {
            state->grpc_ctx->TryCancel();
            state->grpc_ctx.reset();
          }
          conn.send_text("{\"type\":\"unsubscribed\",\"asset\":\"" + asset + "\"}");

        } else if (action == "ping") {
          conn.send_text("{\"type\":\"pong\"}");
        }
      })
      .onclose([](crow::websocket::connection& conn, const std::string&, uint16_t) {
        auto* state = static_cast<MarketDataWsState*>(conn.userdata());
        if (state) {
          state->streaming = false;
          if (state->grpc_ctx) state->grpc_ctx->TryCancel();
          delete state;
          conn.userdata(nullptr);
        }
      })
      .onerror([](crow::websocket::connection& conn, const std::string&) {
        auto* state = static_cast<MarketDataWsState*>(conn.userdata());
        if (state) {
          state->streaming = false;
          if (state->grpc_ctx) state->grpc_ctx->TryCancel();
          delete state;
          conn.userdata(nullptr);
        }
      });

  // ─── F-05 Live Market Data REST endpoints ───────────────────────────────────

  // GET /api/v1/marketdata/{asset} — текущий snapshot, p95 < 50ms
  CROW_ROUTE(app, "/api/v1/marketdata/<string>").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& /*req*/, const std::string& asset) {
        const auto resp = market_data_.GetMarketDataSnapshot(asset);
        if (!resp.found()) {
          crow::json::wvalue err;
          err["error"] = resp.has_error() ? resp.error().message() : "not found";
          return crow::response(404, err);
        }
        const auto& snap = resp.snapshot();
        // Конвертируем Decimal в double для читаемого JSON (scale=8 у всех полей)
        auto to_f = [](const fob::common::v1::Decimal& d) -> double {
          if (d.scale() == 0) return static_cast<double>(d.units());
          return static_cast<double>(d.units()) / std::pow(10.0, d.scale());
        };
        auto depth_json = [&to_f](const auto& levels) {
          crow::json::wvalue arr(crow::json::type::List);
          size_t i = 0;
          for (const auto& l : levels) {
            arr[i]["price"] = to_f(l.price());
            arr[i]["qty"]   = to_f(l.qty());
            ++i;
          }
          return arr;
        };
        crow::json::wvalue out;
        out["asset"]            = snap.asset();
        out["mid"]              = to_f(snap.mid());
        out["best_bid"]         = to_f(snap.best_bid());
        out["best_ask"]         = to_f(snap.best_ask());
        out["spread"]           = to_f(snap.spread());
        out["spread_bps"]       = to_f(snap.spread_bps());
        out["volume_24h"]       = to_f(snap.volume_24h());
        out["volume_quote_24h"] = to_f(snap.volume_quote_24h());
        out["clear_price"]      = to_f(snap.clear_price());
        out["executed_rate"]    = to_f(snap.executed_rate());
        out["bid_depth"]        = depth_json(snap.bid_depth());
        out["ask_depth"]        = depth_json(snap.ask_depth());
        out["source"]           = snap.source();
        out["stale"]            = snap.stale();
        out["batch_id"]         = snap.batch_id();
        out["snapshot_id"]      = snap.snapshot_id();
        return crow::response(200, out);
      });

  // GET /api/v1/marketdata/{asset}/history?limit=N — исторические снимки
  CROW_ROUTE(app, "/api/v1/marketdata/<string>/history").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req, const std::string& asset) {
        const int limit = [&] {
          const auto lp = req.url_params.get("limit");
          if (!lp) return 100;
          try { return std::stoi(lp); } catch (...) { return 100; }
        }();
        const auto parse_unix = [&](const char* name) -> int64_t {
          const auto v = req.url_params.get(name);
          if (!v) return 0;
          try { return std::stoll(v); } catch (...) { return 0; }
        };
        const auto resp = market_data_.GetMarketDataHistory(
            asset, std::min(limit, 1000), parse_unix("from"), parse_unix("to"));
        // Same Decimal→double scaling as the live /marketdata/{asset} endpoint,
        // so history mid/spread_bps share its magnitude (not raw .units()).
        auto to_f = [](const fob::common::v1::Decimal& d) -> double {
          if (d.scale() == 0) return static_cast<double>(d.units());
          return static_cast<double>(d.units()) / std::pow(10.0, d.scale());
        };
        crow::json::wvalue out;
        out["asset"] = asset;
        std::vector<crow::json::wvalue> items;
        for (const auto& snap : resp.snapshots()) {
          crow::json::wvalue s;
          s["snapshot_id"] = snap.snapshot_id();
          s["mid"]         = to_f(snap.mid());
          s["spread_bps"]  = to_f(snap.spread_bps());
          s["source"]      = snap.source();
          s["batch_id"]    = snap.batch_id();
          items.push_back(std::move(s));
        }
        out["snapshots"] = std::move(items);
        out["count"]     = static_cast<int>(resp.snapshots_size());
        return crow::response(200, out);
      });

  // GET /api/v1/marketdata/{asset}/effective-spread?limit=N&from=unix&to=unix
  CROW_ROUTE(app, "/api/v1/marketdata/<string>/effective-spread").methods(crow::HTTPMethod::Get)(
      [this](const crow::request& req, const std::string& asset) {
        fob::marketdata::v1::GetEffectiveSpreadRequest grpc_req;
        grpc_req.mutable_meta()->set_source("gateway");
        grpc_req.set_asset(asset);
        const int limit = [&] {
          const auto lp = req.url_params.get("limit");
          if (!lp) return 100;
          try { return std::stoi(lp); } catch (...) { return 100; }
        }();
        grpc_req.set_limit(static_cast<uint32_t>(std::min(limit, 1000)));
        const auto parse_unix = [&](const char* name) -> int64_t {
          const auto v = req.url_params.get(name);
          if (!v) return 0;
          try { return std::stoll(v); } catch (...) { return 0; }
        };
        if (const int64_t f = parse_unix("from"); f > 0) grpc_req.mutable_from()->set_seconds(f);
        if (const int64_t t = parse_unix("to");   t > 0) grpc_req.mutable_to()->set_seconds(t);

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
        fob::marketdata::v1::GetEffectiveSpreadResponse resp;
        const auto status = market_data_.GetEffectiveSpreadDirect(grpc_req, &resp);
        if (!status.ok()) {
          crow::json::wvalue err;
          err["error"] = status.error_message();
          return crow::response(503, err);
        }

        // Scaled floats, consistent with the snapshot/history endpoints.
        auto to_f = [](const fob::common::v1::Decimal& d) -> double {
          if (d.scale() == 0) return static_cast<double>(d.units());
          return static_cast<double>(d.units()) / std::pow(10.0, d.scale());
        };
        crow::json::wvalue out;
        out["asset"] = asset;
        std::vector<crow::json::wvalue> items;
        for (const auto& r : resp.records()) {
          crow::json::wvalue rec;
          rec["fill_id"]               = r.fill_id();
          rec["effective_spread_bps"]  = to_f(r.effective_spread_bps());
          rec["exec_price"]            = to_f(r.exec_price());
          rec["mid_at_exec"]           = to_f(r.mid_at_exec());
          rec["batch_id"]              = r.batch_id();
          items.push_back(std::move(rec));
        }
        out["records"] = std::move(items);
        out["count"]   = static_cast<int>(resp.records_size());
        return crow::response(200, out);
      });

  // T-F15-007: size the Crow worker pool explicitly. Default 64 leaves ample
  // headroom for REST once SSE streams are capped (REPLAY_SSE_MAX_STREAMS=32),
  // instead of multithreaded()'s hardware_concurrency (often 4-8) which a
  // handful of long-poll streams could exhaust.
  const int gw_threads = std::max(
      4, cex::common::Env::get_int("REPLAY_GW_THREADS", 64));
  cex::common::log_json("INFO", "Gateway starting",
                        {{"port", std::to_string(port)},
                         {"threads", std::to_string(gw_threads)}});
  app.port(port)
      .concurrency(static_cast<std::uint16_t>(gw_threads))
      .run();
}

}  // namespace cex::gateway::transport
