#include "app/create_replay_session_uc.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "cex/common/uuid.hpp"
#include "app/rbac_engine.hpp"  // <-- ADDED

namespace cex::backtest::app {
namespace {

using json = nlohmann::json;

constexpr const char* kValidationError = "validation_error";
constexpr const char* kConfigError = "config_error";
constexpr const char* kDependencyError = "dependency_error";
constexpr const char* kRepositoryError = "repository_error";
constexpr const char* kPermissionDenied = "permission_denied";  // <-- ADDED
constexpr int64_t kHistoryPreflightChunkSize = 10000;

CreateReplaySession::Result ErrorResult(std::string code, std::string message) {
  CreateReplaySession::Result result;
  result.ok = false;
  result.error_code = std::move(code);
  result.error_message = std::move(message);
  return result;
}

bool IsBlank(const std::string& value) {
  return value.empty() ||
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isspace(c) != 0;
         });
}

int64_t ToEpochMs(const std::chrono::system_clock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             tp.time_since_epoch())
      .count();
}

std::optional<int32_t> PreflightHistoricalBatches(
    IHistoricalBatchLoader* loader,
    const std::chrono::system_clock::time_point from,
    const std::chrono::system_clock::time_point to) {
  if (loader == nullptr) return std::nullopt;

  const int64_t from_ms = ToEpochMs(from);
  const int64_t to_ms = ToEpochMs(to);
  int64_t offset = 0;
  std::unordered_set<std::string> unique_batch_ids;
  while (true) {
    const auto chunk =
        loader->LoadBatchResults(from_ms, to_ms, offset, kHistoryPreflightChunkSize);
    const int64_t got = static_cast<int64_t>(chunk.size());
    for (const auto& row : chunk) {
      if (!row.batch_id.empty()) unique_batch_ids.insert(row.batch_id);
    }
    if (got < kHistoryPreflightChunkSize) break;
    offset += got;
    if (unique_batch_ids.size() >= static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      return std::numeric_limits<int32_t>::max();
    }
  }

  // F15 requires the create chain to hit all historical sources. These calls
  // are preflight probes; RunReplaySession still performs the authoritative
  // deterministic range load and detailed consistency validation.
  (void)loader->LoadFills(from_ms, to_ms, 0, 1);
  (void)loader->LoadMarketdataSnapshots(from_ms, to_ms, 0, 1);

  return static_cast<int32_t>(std::min<size_t>(
      unique_batch_ids.size(), static_cast<size_t>(std::numeric_limits<int32_t>::max())));
}

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string NormalizeRewardMode(std::string raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char ch : raw) {
    if (ch == '_' || ch == '-' || std::isspace(ch) != 0) continue;
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

bool IsSupportedRewardMode(const std::string& raw) {
  const std::string mode = NormalizeRewardMode(raw);
  return mode == "incrementalpnl" || mode == "is" ||
         mode == "negativeis" || mode == "minusis" ||
         mode == "shortfall";
}

bool CanonicalizeJson(const std::string& raw,
                      const std::string& field_name,
                      bool require_object,
                      std::string* normalized_json,
                      std::string* error) {
  const json parsed = json::parse(raw, nullptr, false);
  if (parsed.is_discarded()) {
    *error = field_name + " is not valid JSON";
    return false;
  }
  if (require_object && !parsed.is_object()) {
    *error = field_name + " must be a JSON object";
    return false;
  }
  *normalized_json = parsed.dump();
  return true;
}

bool RequireNumber(const json& object,
                   const char* key,
                   const std::string& field_name,
                   const double min_value,
                   const bool inclusive_min,
                   std::string* error) {
  if (!object.contains(key) || object[key].is_null()) {
    *error = field_name + "." + key + " is required";
    return false;
  }
  if (!object[key].is_number()) {
    *error = field_name + "." + key + " must be a number";
    return false;
  }
  const double value = object[key].get<double>();
  if (!std::isfinite(value) ||
      (inclusive_min ? value < min_value : value <= min_value)) {
    *error = field_name + "." + key +
             (inclusive_min ? " must be >= " : " must be > ") +
             std::to_string(min_value);
    return false;
  }
  return true;
}

bool RequireNonNegativeNumber(const json& object,
                              const char* key,
                              const std::string& field_name,
                              std::string* error) {
  if (!object.contains(key) || !object[key].is_number()) {
    *error = field_name + "." + key + " must be a number";
    return false;
  }
  const double value = object[key].get<double>();
  if (!std::isfinite(value) || value < 0.0) {
    *error = field_name + "." + key + " must be >= 0";
    return false;
  }
  return true;
}

bool ValidateFeeModelJson(const json& model,
                          const std::string& field_name,
                          std::string* error) {
  return RequireNonNegativeNumber(model, "makerfeerate", field_name, error) &&
         RequireNonNegativeNumber(model, "takerfeerate", field_name, error);
}

bool ValidateRewardConfigJson(const json& reward,
                              const std::string& field_name,
                              std::string* error) {
  if (!reward.contains("mode") || reward["mode"].is_null()) return true;
  if (!reward["mode"].is_string() ||
      !IsSupportedRewardMode(reward["mode"].get<std::string>())) {
    *error = field_name + ".mode must be incrementalPnL or -IS";
    return false;
  }
  return true;
}

bool ValidateSolverConfigJson(const json& solver,
                              const std::string& field_name,
                              std::string* error) {
  if (!RequireNumber(solver, "batchintervalms", field_name, 0.0, false, error)) {
    return false;
  }
  if (!RequireNumber(solver, "maxiterations", field_name, 0.0, false, error)) {
    return false;
  }
  if (!RequireNumber(solver, "tolerance", field_name, 0.0, false, error)) {
    return false;
  }
  if (!RequireNumber(solver, "epsilonliquidity", field_name, 0.0, true, error)) {
    return false;
  }
  if (solver.contains("feemodel") && !solver["feemodel"].is_null()) {
    if (!solver["feemodel"].is_object()) {
      *error = field_name + ".feemodel must be a JSON object";
      return false;
    }
    return ValidateFeeModelJson(solver["feemodel"], field_name + ".feemodel", error);
  }
  return true;
}

bool ValidateRiskLimitsJson(const json& limits,
                            const std::string& field_name,
                            std::string* error) {
  for (const char* key : {"maxnotional", "maxposition", "maxleverage",
                         "maxorderrate"}) {
    if (!RequireNumber(limits, key, field_name, 0.0, false, error)) {
      return false;
    }
  }
  if (limits.contains("whitelist") && !limits["whitelist"].is_null()) {
    if (!limits["whitelist"].is_array()) {
      *error = field_name + ".whitelist must be an array";
      return false;
    }
    for (size_t i = 0; i < limits["whitelist"].size(); ++i) {
      if (!limits["whitelist"][i].is_string() ||
          IsBlank(limits["whitelist"][i].get<std::string>())) {
        *error = field_name + ".whitelist[" + std::to_string(i) +
                 "] must be a non-empty string";
        return false;
      }
    }
  }
  return true;
}

bool ValidateFlowOrder(const CreateReplaySession::FlowOrderParams& flow,
                       size_t index,
                       std::string* error) {
  const std::string label = "strategy[" + std::to_string(index) + "]";
  if (IsBlank(flow.symbol)) {
    *error = label + ".symbol must not be empty";
    return false;
  }
  const std::string side = ToLowerAscii(flow.side);
  if (side != "buy" && side != "sell") {
    *error = label + ".side must be either 'buy' or 'sell'";
    return false;
  }
  if (flow.pL > flow.pH) {
    *error = label + ".pL must be <= pH";
    return false;
  }
  if (flow.qrate <= 0.0) {
    *error = label + ".qrate must be > 0";
    return false;
  }
  if (flow.qmax <= 0.0) {
    *error = label + ".qmax must be > 0";
    return false;
  }
  if (flow.has_executionwindow && flow.executionwindow <= 0) {
    *error = label + ".executionwindow must be > 0";
    return false;
  }
  return true;
}

bool ReadStringField(const json& row,
                     const char* key,
                     std::string* value,
                     std::string* error,
                     const std::string& field_path) {
  auto it = row.find(key);
  if (it == row.end() || !it->is_string()) {
    *error = field_path + " must be a string";
    return false;
  }
  *value = it->get<std::string>();
  return true;
}

bool ReadDoubleField(const json& row,
                     const char* key,
                     double* value,
                     std::string* error,
                     const std::string& field_path) {
  auto it = row.find(key);
  if (it == row.end() || !it->is_number()) {
    *error = field_path + " must be a number";
    return false;
  }
  *value = it->get<double>();
  return true;
}

bool ReadInt64Field(const json& row,
                    const char* key,
                    int64_t* value,
                    std::string* error,
                    const std::string& field_path) {
  auto it = row.find(key);
  if (it == row.end() || !it->is_number()) {
    *error = field_path + " must be a number";
    return false;
  }
  if (it->is_number_integer() || it->is_number_unsigned()) {
    *value = it->get<int64_t>();
    return true;
  }
  const double numeric = it->get<double>();
  if (!std::isfinite(numeric) || std::floor(numeric) != numeric) {
    *error = field_path + " must be an integer";
    return false;
  }
  *value = static_cast<int64_t>(numeric);
  return true;
}

bool NormalizeStrategyFromTyped(const CreateReplaySession::Request& request,
                                std::vector<CreateReplaySession::FlowOrderParams>* out_strategy,
                                std::string* strategy_json,
                                std::string* error) {
  if (request.strategy.empty()) {
    *error = "strategy must not be empty";
    return false;
  }

  json serialized = json::array();
  out_strategy->clear();
  out_strategy->reserve(request.strategy.size());

  for (size_t i = 0; i < request.strategy.size(); ++i) {
    auto flow = request.strategy[i];
    flow.side = ToLowerAscii(flow.side);
    if (!ValidateFlowOrder(flow, i, error)) {
      return false;
    }

    json entry = {
        {"symbol", flow.symbol},
        {"side", flow.side},
        {"pL", flow.pL},
        {"pH", flow.pH},
        {"qrate", flow.qrate},
        {"qmax", flow.qmax},
        {"executionwindow", flow.executionwindow},
    };
    if (request.instrument.has_value() && !request.instrument->empty()) {
      entry["instrument"] = *request.instrument;
    }
    serialized.push_back(std::move(entry));
    out_strategy->push_back(std::move(flow));
  }

  *strategy_json = serialized.dump();
  return true;
}

bool NormalizeStrategyFromJson(const CreateReplaySession::Request& request,
                               std::vector<CreateReplaySession::FlowOrderParams>* out_strategy,
                               std::string* strategy_json,
                               std::string* error) {
  if (!request.strategy_json.has_value() || request.strategy_json->empty()) {
    *error = "strategy_json must not be empty";
    return false;
  }

  json parsed = json::parse(*request.strategy_json, nullptr, false);
  if (parsed.is_discarded()) {
    *error = "strategy_json is not valid JSON";
    return false;
  }
  if (!parsed.is_array()) {
    *error = "strategy_json must be a JSON array";
    return false;
  }
  if (parsed.empty()) {
    *error = "strategy must not be empty";
    return false;
  }

  out_strategy->clear();
  out_strategy->reserve(parsed.size());

  for (size_t i = 0; i < parsed.size(); ++i) {
    const std::string item = "strategy[" + std::to_string(i) + "]";
    if (!parsed[i].is_object()) {
      *error = item + " must be a JSON object";
      return false;
    }
    const auto& row = parsed[i];
    CreateReplaySession::FlowOrderParams flow;
    if (!ReadStringField(row, "symbol", &flow.symbol, error, item + ".symbol")) {
      return false;
    }
    if (!ReadStringField(row, "side", &flow.side, error, item + ".side")) {
      return false;
    }
    if (!ReadDoubleField(row, "pL", &flow.pL, error, item + ".pL")) {
      return false;
    }
    if (!ReadDoubleField(row, "pH", &flow.pH, error, item + ".pH")) {
      return false;
    }
    if (!ReadDoubleField(row, "qrate", &flow.qrate, error, item + ".qrate")) {
      return false;
    }
    if (!ReadDoubleField(row, "qmax", &flow.qmax, error, item + ".qmax")) {
      return false;
    }
    flow.has_executionwindow = row.contains("executionwindow");
    if (flow.has_executionwindow) {
      if (!ReadInt64Field(row, "executionwindow", &flow.executionwindow, error,
                          item + ".executionwindow")) {
        return false;
      }
    }

    flow.side = ToLowerAscii(flow.side);
    if (!ValidateFlowOrder(flow, i, error)) {
      return false;
    }

    out_strategy->push_back(flow);
    parsed[i]["side"] = flow.side;
    if (request.instrument.has_value() && !request.instrument->empty()) {
      parsed[i]["instrument"] = *request.instrument;
    }
  }

  *strategy_json = parsed.dump();
  return true;
}

}  // namespace

CreateReplaySession::CreateReplaySession(Dependencies deps,
                                         Clock clock,
                                         IdGenerator id_generator)
    : deps_(std::move(deps)),
      clock_(std::move(clock)),
      id_generator_(std::move(id_generator)) {
  if (!clock_) {
    clock_ = []() { return std::chrono::system_clock::now(); };
  }
  if (!id_generator_) {
    id_generator_ = []() { return cex::common::uuid_v4(); };
  }
}

std::chrono::system_clock::time_point CreateReplaySession::Now() const {
  return clock_();
}

std::string CreateReplaySession::GenerateId() const {
  return id_generator_();
}

CreateReplaySession::Result CreateReplaySession::Run(const Request& request) const {
  if (deps_.session_repo == nullptr || deps_.config_builder == nullptr) {
    return ErrorResult(kDependencyError, "CreateReplaySession dependencies are missing");
  }

  // ========== RBAC CHECK - ADDED ==========
  if (deps_.rbac_engine) {
    auto auth = deps_.rbac_engine->CanCreateReplaySession(request.user_id);
    if (!auth.allowed) {
      return ErrorResult(kPermissionDenied,
                         auth.reason.value_or("User cannot create replay sessions"));
    }
  }
  // ========== END RBAC CHECK ==========

  if (IsBlank(request.user_id)) {
    return ErrorResult(kValidationError, "user_id is required");
  }
  if (IsBlank(request.name)) {
    return ErrorResult(kValidationError, "name is required");
  }
  if (request.date_range_from > request.date_range_to) {
    return ErrorResult(kValidationError, "date_range_from must be <= date_range_to");
  }
  if (request.strategy_json.has_value() && !request.strategy.empty()) {
    return ErrorResult(
        kValidationError,
        "provide either strategy_json or typed strategy, but not both");
  }
  if (IsBlank(request.solver_config_id) &&
      !request.solver_config_inline_json.has_value()) {
    return ErrorResult(kValidationError, "solver_config_id is required");
  }
  if (IsBlank(request.risk_limits_id) &&
      !request.risk_limits_inline_json.has_value()) {
    return ErrorResult(kValidationError, "risk_limits_id is required");
  }
  if (request.instrument.has_value() && IsBlank(*request.instrument)) {
    return ErrorResult(kValidationError, "instrument must not be blank");
  }

  std::vector<FlowOrderParams> normalized_strategy;
  std::string strategy_json;
  std::string error;
  if (request.strategy_json.has_value()) {
    if (!NormalizeStrategyFromJson(request, &normalized_strategy, &strategy_json, &error)) {
      return ErrorResult(kValidationError, error);
    }
  } else {
    if (!NormalizeStrategyFromTyped(request, &normalized_strategy, &strategy_json, &error)) {
      return ErrorResult(kValidationError, error);
    }
  }

  std::optional<int32_t> historical_total_batches;
  try {
    historical_total_batches = PreflightHistoricalBatches(
        deps_.historical_loader, request.date_range_from, request.date_range_to);
  } catch (const std::exception& ex) {
    (void)ex;
    historical_total_batches = std::nullopt;
  } catch (...) {
    historical_total_batches = std::nullopt;
  }

  std::optional<std::string> solver_config_inline_override;
  if (request.solver_config_inline_json.has_value()) {
    std::string canonical_solver;
    if (!CanonicalizeJson(*request.solver_config_inline_json,
                          "solver_config_inline_json", true,
                          &canonical_solver, &error)) {
      return ErrorResult(kValidationError, error);
    }
    const auto parsed_solver = json::parse(canonical_solver);
    if (!ValidateSolverConfigJson(parsed_solver, "solver_config_inline_json", &error)) {
      return ErrorResult(kValidationError, error);
    }
    solver_config_inline_override = std::move(canonical_solver);
  }

  std::optional<std::string> risk_limits_inline_override;
  if (request.risk_limits_inline_json.has_value()) {
    std::string canonical_risk;
    if (!CanonicalizeJson(*request.risk_limits_inline_json,
                          "risk_limits_inline_json", true,
                          &canonical_risk, &error)) {
      return ErrorResult(kValidationError, error);
    }
    const auto parsed_risk = json::parse(canonical_risk);
    if (!ValidateRiskLimitsJson(parsed_risk, "risk_limits_inline_json", &error)) {
      return ErrorResult(kValidationError, error);
    }
    risk_limits_inline_override = std::move(canonical_risk);
  }

  std::string fee_model_id = request.fee_model_id.value_or("");
  std::string fee_model_json;
  std::optional<std::string> fee_model_inline_override;
  if (request.fee_model_inline_json.has_value()) {
    if (!CanonicalizeJson(*request.fee_model_inline_json, "fee_model_inline_json",
                          true, &fee_model_json, &error)) {
      return ErrorResult(kValidationError, error);
    }
    const auto parsed_fee = json::parse(fee_model_json);
    if (!ValidateFeeModelJson(parsed_fee, "fee_model_inline_json", &error)) {
      return ErrorResult(kValidationError, error);
    }
    fee_model_inline_override = fee_model_json;
  } else {
    if (IsBlank(fee_model_id)) {
      return ErrorResult(
          kValidationError,
          "fee_model_id or fee_model_inline_json must be provided");
    }
    const json fee_id = {{"id", fee_model_id}};
    fee_model_json = fee_id.dump();
  }

  std::string reward_config_id = request.reward_config_id.value_or("");
  std::optional<std::string> reward_config_inline_override;
  if (request.reward_config_inline_json.has_value()) {
    std::string canonical_reward;
    if (!CanonicalizeJson(*request.reward_config_inline_json, "reward_config_inline_json",
                          true, &canonical_reward, &error)) {
      return ErrorResult(kValidationError, error);
    }
    const auto parsed_reward = json::parse(canonical_reward);
    if (!ValidateRewardConfigJson(parsed_reward, "reward_config_inline_json", &error)) {
      return ErrorResult(kValidationError, error);
    }
    reward_config_inline_override = std::move(canonical_reward);
  }
  if (request.reward_mode.has_value()) {
    if (IsBlank(*request.reward_mode)) {
      return ErrorResult(kValidationError, "reward_mode must not be blank");
    }
    if (!IsSupportedRewardMode(*request.reward_mode)) {
      return ErrorResult(kValidationError, "reward_mode must be incrementalPnL or -IS");
    }
    json reward_json = reward_config_inline_override.has_value()
                           ? json::parse(*reward_config_inline_override)
                           : json::object();
    reward_json["mode"] = *request.reward_mode;
    reward_config_inline_override = reward_json.dump();
  }
  if (IsBlank(reward_config_id) && !reward_config_inline_override.has_value()) {
    return ErrorResult(kValidationError,
                       "reward_config_id, reward_mode or reward_config_inline_json must be provided");
  }

  ReplayConfigRequest config_request;
  config_request.solver_config_id =
      IsBlank(request.solver_config_id) ? "inline" : request.solver_config_id;
  config_request.risk_limits_id =
      IsBlank(request.risk_limits_id) ? "inline" : request.risk_limits_id;
  config_request.fee_model_id = fee_model_id;
  config_request.reward_config_id = reward_config_id;
  config_request.solver_config_inline_override = solver_config_inline_override;
  config_request.risk_limits_inline_override = risk_limits_inline_override;
  config_request.fee_model_inline_override = fee_model_inline_override;
  config_request.reward_config_inline_override = reward_config_inline_override;
  config_request.random_seed = request.random_seed;
  config_request.tolerance = request.tolerance;

  SnapshotResult snapshot_result;
  try {
    snapshot_result = deps_.config_builder->Build(config_request);
  } catch (const std::exception& ex) {
    return ErrorResult(kDependencyError,
                       std::string("Config snapshot builder failed: ") + ex.what());
  } catch (...) {
    return ErrorResult(kDependencyError, "Config snapshot builder failed");
  }

  if (!snapshot_result.ok) {
    return ErrorResult(kConfigError, snapshot_result.error);
  }

  ReplaySession pending;
  pending.session_id = request.session_id.has_value() && !IsBlank(*request.session_id)
                           ? *request.session_id
                           : GenerateId();
  if (IsBlank(pending.session_id)) {
    return ErrorResult(kDependencyError, "id generator returned empty session id");
  }
  pending.user_id = request.user_id;
  pending.name = request.name;
  pending.strategy_json = std::move(strategy_json);
  pending.date_range_from = request.date_range_from;
  pending.date_range_to = request.date_range_to;
  pending.solver_config_id = config_request.solver_config_id;
  pending.risk_limits_id = config_request.risk_limits_id;
  pending.fee_model_json = std::move(fee_model_json);
  pending.session_config_snapshot_json = snapshot_result.snapshot.snapshot_json;
  pending.status = ReplaySessionStatus::kPending;
  pending.total_batches = historical_total_batches;
  pending.progress_batches = 0;
  pending.created_at = Now();
  pending.started_at = std::nullopt;
  pending.completed_at = std::nullopt;
  pending.error_details = std::nullopt;

  try {
    Result result;
    result.ok = true;
    result.created = deps_.session_repo->Create(pending);
    return result;
  } catch (const std::exception& ex) {
    return ErrorResult(kRepositoryError,
                       std::string("Replay session create failed: ") + ex.what());
  } catch (...) {
    return ErrorResult(kRepositoryError, "Replay session create failed");
  }
}

}  // namespace cex::backtest::app
