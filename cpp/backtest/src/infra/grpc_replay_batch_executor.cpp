#include "infra/grpc_replay_batch_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <google/protobuf/empty.pb.h>
#include <nlohmann/json.hpp>

#include "cex/common/decimal.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/matching/v1/batch.pb.h"

namespace cex::backtest::infra {
namespace {

using json = nlohmann::json;

constexpr int32_t kDecimalScale = 8;

double DecimalToDouble(const fob::common::v1::Decimal& value) {
  return static_cast<double>(cex::common::Decimal::from_proto(value));
}

fob::common::v1::Decimal DecimalFromDouble(double value) {
  const double factor = std::pow(10.0, static_cast<double>(kDecimalScale));
  cex::common::Decimal decimal{
      static_cast<int64_t>(std::llround(value * factor)), kDecimalScale};
  return decimal.to_proto();
}

double ParseDoubleString(const std::string& value) {
  if (value.empty()) return 0.0;
  try {
    return std::stod(value);
  } catch (...) {
    return 0.0;
  }
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string NormalizeSymbol(std::string value) {
  value.erase(std::remove(value.begin(), value.end(), '/'), value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

std::pair<std::string, std::string> SplitSymbol(const std::string& symbol) {
  const auto slash = symbol.find('/');
  if (slash != std::string::npos) {
    return {symbol.substr(0, slash), symbol.substr(slash + 1)};
  }
  static constexpr const char* kQuotes[] = {"USDT", "USDC", "USD", "BTC", "ETH", "EUR"};
  for (const char* quote : kQuotes) {
    const std::string q(quote);
    if (symbol.size() > q.size() &&
        symbol.compare(symbol.size() - q.size(), q.size(), q) == 0) {
      return {symbol.substr(0, symbol.size() - q.size()), q};
    }
  }
  return {symbol, ""};
}

std::optional<double> JsonDouble(const json& node,
                                 std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (!node.contains(key) || node.at(key).is_null()) continue;
    const auto& value = node.at(key);
    if (value.is_number()) return value.get<double>();
    if (value.is_string()) {
      try {
        return std::stod(value.get<std::string>());
      } catch (...) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string> JsonString(const json& node,
                                      std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (!node.contains(key) || node.at(key).is_null()) continue;
    const auto& value = node.at(key);
    if (value.is_string()) return value.get<std::string>();
  }
  return std::nullopt;
}

std::vector<double> ParseDoubleArray(const std::string& raw_json) {
  std::vector<double> values;
  if (raw_json.empty()) return values;
  const auto parsed = json::parse(raw_json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_array()) return values;
  values.reserve(parsed.size());
  for (const auto& item : parsed) {
    if (item.is_number()) {
      values.push_back(item.get<double>());
    } else if (item.is_string()) {
      try {
        values.push_back(std::stod(item.get<std::string>()));
      } catch (...) {
      }
    }
  }
  return values;
}

void FillRepeatedDoubles(const std::vector<double>& values,
                         google::protobuf::RepeatedField<double>* out) {
  out->Clear();
  for (double value : values) out->Add(value);
}

fob::venue::v1::VenueLiquidityCurve CurveRowToProto(const app::CurveRow& row) {
  fob::venue::v1::VenueLiquidityCurve curve;
  curve.mutable_meta()->set_source("backtest.replay.historical_curve");
  curve.mutable_timestamp()->set_seconds(row.event_time_ms / 1000);
  curve.mutable_timestamp()->set_nanos(
      static_cast<int32_t>((row.event_time_ms % 1000) * 1000000));
  curve.set_venue_id(row.venue_id);
  curve.mutable_instrument()->set_symbol(row.symbol);
  const auto [base, quote] = SplitSymbol(row.symbol);
  curve.mutable_instrument()->set_base(base);
  curve.mutable_instrument()->set_quote(quote);
  curve.set_snapshot_id(row.snapshot_id);
  curve.set_curve_id(row.curve_id);
  curve.set_level(row.level);
  curve.set_epsilon1(row.epsilon1);
  curve.set_epsilon2(row.epsilon2);
  curve.set_epsilon3(row.epsilon3);
  curve.set_confidence(row.confidence);
  *curve.mutable_mid_price() = DecimalFromDouble(row.mid_price);
  curve.set_tau_ms(row.tau_ms);

  FillRepeatedDoubles(ParseDoubleArray(row.bid_q_grid),
                      curve.mutable_bid_curve()->mutable_q_grid());
  FillRepeatedDoubles(ParseDoubleArray(row.bid_p_of_q),
                      curve.mutable_bid_curve()->mutable_p_of_q());
  FillRepeatedDoubles(ParseDoubleArray(row.bid_s_of_q),
                      curve.mutable_bid_curve()->mutable_s_of_q());
  FillRepeatedDoubles(ParseDoubleArray(row.ask_q_grid),
                      curve.mutable_ask_curve()->mutable_q_grid());
  FillRepeatedDoubles(ParseDoubleArray(row.ask_p_of_q),
                      curve.mutable_ask_curve()->mutable_p_of_q());
  FillRepeatedDoubles(ParseDoubleArray(row.ask_s_of_q),
                      curve.mutable_ask_curve()->mutable_s_of_q());
  return curve;
}

bool PreferCurve(const app::CurveRow& candidate, const app::CurveRow& current) {
  if (candidate.confidence > current.confidence + 1e-9) return true;
  if (std::fabs(candidate.confidence - current.confidence) <= 1e-9 &&
      candidate.event_time_ms > current.event_time_ms) {
    return true;
  }
  return false;
}

std::vector<std::string> SymbolLookupVariants(const std::string& symbol) {
  std::vector<std::string> variants;
  if (symbol.empty()) return variants;
  variants.push_back(symbol);
  const auto [base, quote] = SplitSymbol(symbol);
  if (!base.empty() && !quote.empty()) {
    const std::string slash = base + "/" + quote;
    if (std::find(variants.begin(), variants.end(), slash) == variants.end()) {
      variants.push_back(slash);
    }
    const std::string compact = base + quote;
    if (std::find(variants.begin(), variants.end(), compact) == variants.end()) {
      variants.push_back(compact);
    }
  }
  return variants;
}

void AttachHistoricalExternalLiquidity(app::IReplayReader* replay_reader,
                                       int64_t lookback_ms,
                                       const app::HistoricalBatch& batch,
                                       fob::matching::v1::BatchRequest* solve_req) {
  if (replay_reader == nullptr || solve_req == nullptr) return;
  const int64_t batch_time_ms = batch.batch_result.event_time_ms;
  if (batch_time_ms <= 0) return;

  std::unordered_map<std::string, app::CurveRow> best_by_symbol;
  std::set<std::string> loaded_keys;
  const int64_t from_ms = std::max<int64_t>(0, batch_time_ms - std::max<int64_t>(0, lookback_ms));

  for (const auto& snapshot : batch.marketdata_snapshots) {
    if (snapshot.venue_id.empty() || snapshot.symbol.empty()) continue;
    for (const auto& symbol : SymbolLookupVariants(snapshot.symbol)) {
      const std::string load_key = snapshot.venue_id + "|" + symbol;
      if (!loaded_keys.insert(load_key).second) continue;
      const auto rows = replay_reader->LoadCurves(
          snapshot.venue_id, symbol, from_ms, batch_time_ms);
      for (const auto& row : rows) {
        if (row.symbol.empty() || row.event_time_ms > batch_time_ms) continue;
        const std::string key = NormalizeSymbol(row.symbol);
        if (key.empty()) continue;
        const auto it = best_by_symbol.find(key);
        if (it == best_by_symbol.end() || PreferCurve(row, it->second)) {
          best_by_symbol[key] = row;
        }
      }
    }
  }
  for (const auto& fill : batch.fills) {
    if (fill.venue_id.empty() || fill.symbol.empty()) continue;
    for (const auto& symbol : SymbolLookupVariants(fill.symbol)) {
      const std::string load_key = fill.venue_id + "|" + symbol;
      if (!loaded_keys.insert(load_key).second) continue;
      const auto rows = replay_reader->LoadCurves(
          fill.venue_id, symbol, from_ms, batch_time_ms);
      for (const auto& row : rows) {
        if (row.symbol.empty() || row.event_time_ms > batch_time_ms) continue;
        const std::string key = NormalizeSymbol(row.symbol);
        if (key.empty()) continue;
        const auto it = best_by_symbol.find(key);
        if (it == best_by_symbol.end() || PreferCurve(row, it->second)) {
          best_by_symbol[key] = row;
        }
      }
    }
  }

  for (const auto& [_, row] : best_by_symbol) {
    *solve_req->add_external_liquidity() = CurveRowToProto(row);
  }
}

double SnapshotTolerance(const std::string& snapshot_json) {
  if (snapshot_json.empty()) return 0.0;
  auto parsed = json::parse(snapshot_json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return 0.0;
  return JsonDouble(parsed, {"tolerance"}).value_or(0.0);
}

struct StrategyOrder {
  std::string order_id;
  std::string symbol;
  std::string base;
  std::string quote;
  std::string side;
  double price_low{0.0};
  double price_high{0.0};
  double q_rate{0.0};
  double q_max{0.0};
  std::optional<double> execution_window;
};

std::vector<json> StrategyItems(const json& parsed) {
  if (parsed.is_array()) return parsed.get<std::vector<json>>();
  if (parsed.is_object()) {
    for (const char* key : {"floworders", "flow_orders", "strategy"}) {
      if (parsed.contains(key) && parsed.at(key).is_array()) {
        return parsed.at(key).get<std::vector<json>>();
      }
    }
  }
  return {};
}

std::vector<StrategyOrder> ParseStrategy(const std::string& strategy_json,
                                         const std::string& tracked_user_id,
                                         const std::string& batch_id,
                                         std::string* error) {
  std::vector<StrategyOrder> out;
  const auto parsed = json::parse(strategy_json, nullptr, false);
  if (parsed.is_discarded()) {
    *error = "strategy_json is not valid JSON";
    return out;
  }
  const auto items = StrategyItems(parsed);
  if (items.empty()) {
    *error = "strategy has no FlowOrder entries";
    return out;
  }

  out.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    const json& item = items[i];
    if (!item.is_object()) {
      *error = "strategy FlowOrder must be an object";
      out.clear();
      return out;
    }
    StrategyOrder order;
    order.symbol = JsonString(item, {"symbol", "instrument"}).value_or("");
    order.side = LowerAscii(JsonString(item, {"side"}).value_or(""));
    order.price_low = JsonDouble(item, {"pL", "pl", "price_low", "priceLow"}).value_or(0.0);
    order.price_high = JsonDouble(item, {"pH", "ph", "price_high", "priceHigh"}).value_or(0.0);
    order.q_rate = JsonDouble(item, {"qrate", "q_rate", "max_speed"}).value_or(0.0);
    order.q_max = JsonDouble(item, {"qmax", "q_max", "total_qty"}).value_or(0.0);
    order.execution_window = JsonDouble(
        item, {"executionwindow", "execution_window", "executionWindow"});
    if (order.symbol.empty() || (order.side != "buy" && order.side != "sell") ||
        order.price_low > order.price_high || order.q_rate <= 0.0 ||
        order.q_max <= 0.0 ||
        (order.execution_window.has_value() && *order.execution_window <= 0.0)) {
      *error = "strategy FlowOrder failed F-15 validation";
      out.clear();
      return out;
    }
    auto [base, quote] = SplitSymbol(order.symbol);
    order.base = std::move(base);
    order.quote = std::move(quote);
    order.order_id = "replay:" + tracked_user_id + ":" + batch_id + ":" +
                     std::to_string(i + 1);
    out.push_back(std::move(order));
  }
  return out;
}

fob::common::v1::Side ToProtoSide(const std::string& side) {
  return LowerAscii(side) == "sell" ? fob::common::v1::SIDE_SELL
                                    : fob::common::v1::SIDE_BUY;
}

std::string SideToString(fob::common::v1::Side side) {
  return side == fob::common::v1::SIDE_SELL ? "sell" : "buy";
}

fob::orders::v1::FlowOrder ToProtoFlowOrder(const StrategyOrder& order,
                                            const std::string& tracked_user_id) {
  fob::orders::v1::FlowOrder proto;
  proto.set_order_id(order.order_id);
  proto.set_user_id(tracked_user_id);
  proto.set_account_id(tracked_user_id);
  proto.mutable_instrument()->set_symbol(order.symbol);
  proto.mutable_instrument()->set_base(order.base);
  proto.mutable_instrument()->set_quote(order.quote);
  proto.set_side(ToProtoSide(order.side));
  *proto.mutable_total_qty() = DecimalFromDouble(order.q_max);
  *proto.mutable_remaining_qty() = DecimalFromDouble(order.q_max);
  *proto.mutable_price_low() = DecimalFromDouble(order.price_low);
  *proto.mutable_price_high() = DecimalFromDouble(order.price_high);
  *proto.mutable_max_speed() = DecimalFromDouble(order.q_rate);
  proto.set_status(fob::common::v1::ORDER_STATUS_NEW);
  proto.set_tif(fob::common::v1::TIF_GTC);
  (*proto.mutable_tags())["replay"] = "true";
  if (order.execution_window.has_value()) {
    (*proto.mutable_tags())["executionwindow"] = std::to_string(*order.execution_window);
  }
  return proto;
}

json StrategyActionJson(const std::vector<StrategyOrder>& orders) {
  json action = json::array();
  for (const auto& order : orders) {
    json item = {
        {"symbol", order.symbol},
        {"side", order.side},
        {"pL", order.price_low},
        {"pH", order.price_high},
        {"qrate", order.q_rate},
        {"qmax", order.q_max},
        {"orderid", order.order_id},
    };
    if (order.execution_window.has_value()) {
      item["executionwindow"] = *order.execution_window;
    }
    action.push_back(std::move(item));
  }
  return action;
}

std::string RiskDecisionName(fob::risk::v1::RiskDecision decision) {
  switch (decision) {
    case fob::risk::v1::RISK_DECISION_ACCEPT: return "accept";
    case fob::risk::v1::RISK_DECISION_REJECT: return "reject";
    case fob::risk::v1::RISK_DECISION_RESIZE: return "resize";
    case fob::risk::v1::RISK_DECISION_HALT: return "halt";
    default: return "unspecified";
  }
}

json MarketdataJson(const std::vector<app::HistoricalMarketdataSnapshotRow>& rows) {
  json out = json::array();
  for (const auto& row : rows) {
    out.push_back({
        {"venueid", row.venue_id},
        {"symbol", row.symbol},
        {"eventtimems", row.event_time_ms},
        {"mid", row.mid_price},
        {"spread", row.spread},
        {"bestbid", row.best_bid},
        {"bestask", row.best_ask},
        {"status", row.status},
    });
  }
  return out;
}

json MapJson(const std::map<std::string, std::string>& values) {
  json out = json::object();
  for (const auto& [key, value] : values) out[key] = value;
  return out;
}

json StateJson(const app::ShadowLedgerNamespaceState* before_state,
               const app::ShadowLedgerStepState* before_step,
               const app::HistoricalBatch& batch) {
  json state;
  state["batchid"] = batch.batch_result.batch_id;
  state["originalbatchid"] = batch.batch_result.batch_id;
  state["eventtimems"] = batch.batch_result.event_time_ms;
  state["marketdata"] = MarketdataJson(batch.marketdata_snapshots);
  state["cumPnL"] = before_step == nullptr ? 0.0
                                           : ParseDoubleString(before_step->total_pnl);
  if (before_state != nullptr) {
    state["positions"] = MapJson(before_state->positions);
    state["balances"] = MapJson(before_state->balances);
    state["reportingcurrency"] = before_state->reporting_currency;
  } else {
    state["positions"] = json::object();
    state["balances"] = json::object();
  }
  return state;
}

std::map<std::string, double> MidBySymbol(
    const std::vector<app::HistoricalMarketdataSnapshotRow>& rows) {
  std::map<std::string, double> out;
  for (const auto& row : rows) {
    if (row.mid_price > 0.0) out[NormalizeSymbol(row.symbol)] = row.mid_price;
  }
  return out;
}

std::map<std::string, double> ClearPrices(
    const fob::matching::v1::BatchResult& batch_result) {
  std::map<std::string, double> out;
  for (const auto& [symbol, price] : batch_result.clear_prices()) {
    out[symbol] = DecimalToDouble(price);
  }
  return out;
}

double JsonDecimalValue(const json& value) {
  if (value.is_number()) return value.get<double>();
  if (value.is_string()) {
    try {
      return std::stod(value.get<std::string>());
    } catch (...) {
      return 0.0;
    }
  }
  if (value.is_object()) {
    if (auto direct = JsonDouble(value, {"price", "value", "amount"});
        direct.has_value()) {
      return *direct;
    }
    if (value.contains("units") && value.contains("scale")) {
      return static_cast<double>(value.at("units").get<int64_t>()) *
             std::pow(10.0, -value.at("scale").get<int32_t>());
    }
  }
  return 0.0;
}

std::map<std::string, double> HistoricalClearPrices(const std::string& raw_json) {
  std::map<std::string, double> out;
  const auto parsed = json::parse(raw_json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return out;
  for (auto it = parsed.begin(); it != parsed.end(); ++it) {
    const double value = JsonDecimalValue(it.value());
    if (value > 0.0) out[it.key()] = value;
  }
  return out;
}

json ClearPricesJson(const std::map<std::string, double>& values) {
  json out = json::object();
  for (const auto& [symbol, price] : values) out[symbol] = price;
  return out;
}

json ExecutedRatesJson(const fob::matching::v1::BatchResult& batch_result) {
  json out = json::object();
  for (const auto& [order_id, rate] : batch_result.executed_rates()) {
    out[order_id] = DecimalToDouble(rate);
  }
  return out;
}

json FillJson(const fob::matching::v1::FlowFill& fill) {
  json out = {
      {"orderid", fill.order_id()},
      {"userid", fill.user_id()},
      {"symbol", fill.instrument().symbol()},
      {"base", fill.instrument().base()},
      {"quote", fill.instrument().quote()},
      {"side", SideToString(fill.side())},
      {"execqty", DecimalToDouble(fill.executed_qty())},
      {"execprice", DecimalToDouble(fill.price())},
      {"executednotional", DecimalToDouble(fill.executed_notional())},
      {"liquiditysource", fill.liquidity_source()},
      {"venueid", fill.provenance().venue_id()},
      {"snapshotid", fill.provenance().snapshot_id()},
      {"curveid", fill.provenance().curve_id()},
  };
  if (fill.has_fee()) {
    out["fee"] = {
        {"type", fill.fee().fee_type()},
        {"currency", fill.fee().cost().currency()},
        {"amount", DecimalToDouble(fill.fee().cost().amount())},
    };
  }
  return out;
}

std::vector<app::ShadowLedgerFill> ToShadowFills(
    const fob::matching::v1::BatchResult& batch_result) {
  std::vector<app::ShadowLedgerFill> out;
  out.reserve(batch_result.fills_size());
  for (const auto& fill : batch_result.fills()) {
    app::ShadowLedgerFill shadow;
    shadow.order_id = fill.order_id();
    shadow.user_id = fill.user_id();
    shadow.symbol = fill.instrument().symbol();
    shadow.base = fill.instrument().base();
    shadow.quote = fill.instrument().quote();
    shadow.side = SideToString(fill.side());
    shadow.executed_qty = DecimalToDouble(fill.executed_qty());
    shadow.price = DecimalToDouble(fill.price());
    shadow.executed_notional = DecimalToDouble(fill.executed_notional());
    shadow.liquidity_source = fill.liquidity_source();
    shadow.venue_id = fill.provenance().venue_id();
    shadow.snapshot_id = fill.provenance().snapshot_id();
    shadow.curve_id = fill.provenance().curve_id();
    if (fill.has_fee()) {
      shadow.fee_amount = DecimalToDouble(fill.fee().cost().amount());
      shadow.fee_currency = fill.fee().cost().currency();
    }
    out.push_back(std::move(shadow));
  }
  return out;
}

app::BatchExecutionResult Failure(app::BatchOutcome outcome,
                                  app::FailureComponent component,
                                  std::string code,
                                  std::string message) {
  app::BatchExecutionResult result;
  result.outcome = outcome;
  result.failure_component = component;
  result.error_code = std::move(code);
  result.error_details = std::move(message);
  result.risk_status = outcome == app::BatchOutcome::kOk
                           ? "ok"
                           : outcome == app::BatchOutcome::kSoftFailure ? "soft"
                                                                        : "hard";
  return result;
}

}  // namespace

GrpcReplayBatchExecutor::GrpcReplayBatchExecutor(
    std::unique_ptr<fob::matching::v1::Solver::StubInterface> solver,
    std::unique_ptr<fob::risk::v1::RiskService::StubInterface> risk,
    app::IShadowLedger* shadow_ledger,
    Options options,
    app::IReplayReader* replay_reader)
    : solver_(std::move(solver)),
      risk_(std::move(risk)),
      shadow_ledger_(shadow_ledger),
      options_(options),
      replay_reader_(replay_reader) {}

app::BatchExecutionResult GrpcReplayBatchExecutor::ExecuteBatch(
    const std::string& namespace_id,
    const std::string& session_config_snapshot_json,
    const std::string& strategy_json,
    const std::string& tracked_user_id,
    const std::string& reporting_currency,
    const app::HistoricalBatch& batch) {
  if (solver_ == nullptr || risk_ == nullptr || shadow_ledger_ == nullptr) {
    return Failure(app::BatchOutcome::kHardFailure, app::FailureComponent::kPipeline,
                   "dependency_error",
                   "GrpcReplayBatchExecutor dependencies are not configured");
  }
  if (tracked_user_id.empty()) {
    return Failure(app::BatchOutcome::kHardFailure, app::FailureComponent::kPipeline,
                   "missing_tracked_user",
                   "tracked_user_id is required for replay batch execution");
  }

  std::string strategy_error;
  const auto strategy = ParseStrategy(strategy_json, tracked_user_id,
                                      batch.batch_result.batch_id,
                                      &strategy_error);
  if (strategy.empty()) {
    return Failure(app::BatchOutcome::kHardFailure, app::FailureComponent::kConfig,
                   "invalid_strategy", strategy_error);
  }

  const auto before_state = shadow_ledger_->GetNamespace(namespace_id);
  const auto before_step = shadow_ledger_->GetLastStep(namespace_id);
  const double total_pnl_before =
      before_step.has_value() ? ParseDoubleString(before_step->total_pnl) : 0.0;

  fob::matching::v1::BatchRequest solve_req;
  solve_req.set_batch_id(batch.batch_result.batch_id);
  solve_req.mutable_meta()->set_correlation_id(batch.batch_result.correlation_id);
  solve_req.mutable_meta()->set_source("backtest.replay");
  solve_req.mutable_meta()->set_partition_key(batch.batch_result.partition_key);
  solve_req.set_solver_config_snapshot_json(session_config_snapshot_json);
  const auto mids = MidBySymbol(batch.marketdata_snapshots);
  for (const auto& [symbol, mid] : mids) {
    (*solve_req.mutable_reference_prices())[symbol] = DecimalFromDouble(mid);
  }
  if (solve_req.reference_prices().empty()) {
    for (const auto& [symbol, clear] :
         HistoricalClearPrices(batch.batch_result.clear_prices_json)) {
      (*solve_req.mutable_reference_prices())[symbol] = DecimalFromDouble(clear);
    }
  }
  for (const auto& order : strategy) {
    *solve_req.add_flow_orders() = ToProtoFlowOrder(order, tracked_user_id);
  }
  AttachHistoricalExternalLiquidity(
      replay_reader_, options_.historical_curve_lookback_ms, batch, &solve_req);

  for (int i = 0; i < solve_req.flow_orders_size(); ++i) {
    auto* flow_order = solve_req.mutable_flow_orders(i);
    grpc::ClientContext pre_risk_ctx;
    if (options_.rpc_timeout_ms > 0) {
      pre_risk_ctx.set_deadline(std::chrono::system_clock::now() +
                                std::chrono::milliseconds(options_.rpc_timeout_ms));
    }
    fob::risk::v1::PreTradeCheckRequest pre_risk_req;
    pre_risk_req.mutable_meta()->set_correlation_id(
        batch.batch_result.correlation_id);
    pre_risk_req.mutable_meta()->set_source("backtest.replay.pretrade");
    pre_risk_req.mutable_meta()->set_partition_key(
        batch.batch_result.partition_key);
    pre_risk_req.set_user_id(tracked_user_id);
    *pre_risk_req.mutable_order() = *flow_order;
    const auto normalized_symbol = NormalizeSymbol(flow_order->instrument().symbol());
    if (auto mid_it = mids.find(normalized_symbol); mid_it != mids.end()) {
      *pre_risk_req.mutable_reference_price() = DecimalFromDouble(mid_it->second);
    } else {
      const double fallback_mid =
          (DecimalToDouble(flow_order->price_low()) +
           DecimalToDouble(flow_order->price_high())) /
          2.0;
      if (fallback_mid > 0.0) {
        *pre_risk_req.mutable_reference_price() = DecimalFromDouble(fallback_mid);
      }
    }

    fob::risk::v1::PreTradeCheckResponse pre_risk_resp;
    const auto pre_risk_status =
        risk_->CheckNewOrder(&pre_risk_ctx, pre_risk_req, &pre_risk_resp);
    if (!pre_risk_status.ok()) {
      return Failure(app::BatchOutcome::kHardFailure, app::FailureComponent::kRisk,
                     "risk_pre_trade_unavailable",
                     pre_risk_status.error_message());
    }
    if (pre_risk_resp.decision() == fob::risk::v1::RISK_DECISION_RESIZE &&
        pre_risk_resp.has_resized_order()) {
      *flow_order = pre_risk_resp.resized_order();
      continue;
    }
    if (pre_risk_resp.decision() == fob::risk::v1::RISK_DECISION_REJECT ||
        pre_risk_resp.decision() == fob::risk::v1::RISK_DECISION_HALT) {
      app::BatchExecutionResult rejected;
      rejected.outcome = app::BatchOutcome::kSoftFailure;
      rejected.failure_component = app::FailureComponent::kRisk;
      rejected.error_code = "risk_pre_trade_rejected";
      rejected.error_details =
          pre_risk_resp.has_error() && !pre_risk_resp.error().message().empty()
              ? pre_risk_resp.error().message()
              : "Risk Manager rejected replay order before solver";
      rejected.risk_status = "rejected";
      for (const auto& order : strategy) rejected.requested_qty += order.q_max;
      rejected.state_json = StateJson(before_state ? &*before_state : nullptr,
                                      before_step ? &*before_step : nullptr,
                                      batch)
                                .dump();
      rejected.action_json = StrategyActionJson(strategy).dump();
      rejected.fills_json = json::array().dump();
      rejected.batch_result_json = json{
          {"batchid", batch.batch_result.batch_id},
          {"riskstatus", rejected.risk_status},
          {"riskdecision", RiskDecisionName(pre_risk_resp.decision())},
          {"riskerrorcode", pre_risk_resp.has_error()
                                ? pre_risk_resp.error().code()
                                : std::string{}},
          {"riskerrormessage", rejected.error_details},
      }.dump();
      rejected.metrics_json = json{
          {"requestedqty", rejected.requested_qty},
          {"executedqty", 0.0},
          {"riskdecision", RiskDecisionName(pre_risk_resp.decision())},
      }.dump();
      return rejected;
    }
  }

  fob::matching::v1::BatchResult batch_result;
  grpc::ClientContext solve_ctx;
  if (options_.rpc_timeout_ms > 0) {
    solve_ctx.set_deadline(std::chrono::system_clock::now() +
                           std::chrono::milliseconds(options_.rpc_timeout_ms));
  }
  const auto solve_started = std::chrono::steady_clock::now();
  const auto solve_status = solver_->Solve(&solve_ctx, solve_req, &batch_result);
  const auto elapsed_solve_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - solve_started)
          .count());
  if (!solve_status.ok()) {
    return Failure(app::BatchOutcome::kHardFailure, app::FailureComponent::kSolver,
                   "solver_unavailable", solve_status.error_message());
  }
  if (batch_result.batch_id().empty()) batch_result.set_batch_id(batch.batch_result.batch_id);

  grpc::ClientContext risk_ctx;
  if (options_.rpc_timeout_ms > 0) {
    risk_ctx.set_deadline(std::chrono::system_clock::now() +
                          std::chrono::milliseconds(options_.rpc_timeout_ms));
  }
  fob::risk::v1::PostTradeUpdateRequest risk_req;
  *risk_req.mutable_batch() = batch_result;
  google::protobuf::Empty risk_resp;
  const auto risk_status = risk_->OnBatchResult(&risk_ctx, risk_req, &risk_resp);
  if (!risk_status.ok()) {
    return Failure(app::BatchOutcome::kHardFailure, app::FailureComponent::kRisk,
                   "risk_post_trade_failed", risk_status.error_message());
  }

  auto clear_prices = ClearPrices(batch_result);
  if (clear_prices.empty()) {
    clear_prices = HistoricalClearPrices(batch.batch_result.clear_prices_json);
  }

  app::ShadowLedgerApplyRequest ledger_req;
  ledger_req.namespace_id = namespace_id;
  ledger_req.batch_id = batch.batch_result.batch_id;
  ledger_req.tracked_user_id = tracked_user_id;
  ledger_req.reporting_currency = reporting_currency.empty() ? "USDT" : reporting_currency;
  ledger_req.clear_prices = clear_prices;
  ledger_req.fills = ToShadowFills(batch_result);
  const auto ledger_step = shadow_ledger_->ApplyFills(ledger_req);
  if (!ledger_step.ok) {
    return Failure(app::BatchOutcome::kHardFailure, app::FailureComponent::kLedger,
                   ledger_step.error_code.empty() ? "ledger_apply_failed"
                                                  : ledger_step.error_code,
                   ledger_step.error_message.empty() ? "shadow ledger apply failed"
                                                     : ledger_step.error_message);
  }

  const std::set<std::string> strategy_order_ids = [&strategy] {
    std::set<std::string> ids;
    for (const auto& order : strategy) ids.insert(order.order_id);
    return ids;
  }();
  double requested_qty = 0.0;
  for (const auto& order : strategy) requested_qty += order.q_max;

  double executed_qty = 0.0;
  double executed_notional = 0.0;
  double is_weighted_sum = 0.0;
  double is_weight = 0.0;
  json fills_json = json::array();

  for (const auto& fill : batch_result.fills()) {
    fills_json.push_back(FillJson(fill));
    if (!strategy_order_ids.contains(fill.order_id()) &&
        fill.user_id() != tracked_user_id) {
      continue;
    }
    const double qty = DecimalToDouble(fill.executed_qty());
    const double price = DecimalToDouble(fill.price());
    const double notional = fill.has_executed_notional()
                                ? DecimalToDouble(fill.executed_notional())
                                : qty * price;
    executed_qty += qty;
    executed_notional += notional;

    const std::string normalized_symbol = NormalizeSymbol(fill.instrument().symbol());
    auto mid_it = mids.find(normalized_symbol);
    double decision_price = 0.0;
    if (mid_it != mids.end()) {
      decision_price = mid_it->second;
    } else {
      for (const auto& [symbol, clear] : clear_prices) {
        if (NormalizeSymbol(symbol) == normalized_symbol) {
          decision_price = clear;
          break;
        }
      }
    }
    if (decision_price > 0.0 && qty > 0.0) {
      is_weighted_sum += (price - decision_price) * qty;
      is_weight += qty;
    }
  }

  const double total_pnl_after = ParseDoubleString(ledger_step.total_pnl);
  app::BatchExecutionResult result;
  result.pnl = total_pnl_after - total_pnl_before;
  result.is_value = is_weight > 0.0 ? is_weighted_sum / is_weight : 0.0;
  result.fill_rate = requested_qty > 0.0 ? (executed_qty / requested_qty) * 100.0 : 0.0;
  result.fills_applied = static_cast<uint32_t>(batch_result.fills_size());
  result.solve_time_ms = batch_result.has_diagnostics()
                             ? batch_result.diagnostics().solve_time_ms()
                             : elapsed_solve_ms;
  if (result.solve_time_ms == 0) result.solve_time_ms = elapsed_solve_ms;
  result.residual_norm = batch_result.has_diagnostics()
                             ? batch_result.diagnostics().residual_norm()
                             : batch.batch_result.residual_norm;
  result.vwap = executed_qty > 0.0 ? executed_notional / executed_qty : 0.0;
  result.executed_qty = executed_qty;
  result.requested_qty = requested_qty;
  result.executed_notional = executed_notional;
  result.is_weighted_sum = is_weighted_sum;
  result.is_weight = is_weight;
  result.risk_status = "ok";

  const double tolerance = SnapshotTolerance(session_config_snapshot_json);
  if (tolerance > 0.0 && result.residual_norm > tolerance) {
    result.outcome = app::BatchOutcome::kSoftFailure;
    result.failure_component = app::FailureComponent::kSolver;
    result.error_code = "residual_norm_above_tolerance";
    std::ostringstream msg;
    msg << "solver residual_norm=" << result.residual_norm
        << " exceeds tolerance=" << tolerance;
    result.error_details = msg.str();
    result.risk_status = "soft";
  }

  result.state_json = StateJson(before_state ? &*before_state : nullptr,
                                before_step ? &*before_step : nullptr,
                                batch)
                          .dump();
  result.action_json = StrategyActionJson(strategy).dump();
  result.fills_json = fills_json.dump();
  result.batch_result_json = json{
      {"batchid", batch_result.batch_id()},
      {"clearprices", ClearPricesJson(clear_prices)},
      {"executedrates", ExecutedRatesJson(batch_result)},
      {"residualnorm", result.residual_norm},
      {"solvetimems", result.solve_time_ms},
      {"riskstatus", result.risk_status},
  }.dump();
  result.metrics_json = json{
      {"executedqty", result.executed_qty},
      {"requestedqty", result.requested_qty},
      {"executednotional", result.executed_notional},
      {"isweightedsum", result.is_weighted_sum},
      {"isweight", result.is_weight},
      {"totalpnlbefore", total_pnl_before},
      {"totalpnlafter", total_pnl_after},
  }.dump();

  return result;
}

}  // namespace cex::backtest::infra
