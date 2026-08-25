#include "infra/replay_commands_kafka_consumer.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "cex/common/log.hpp"
#include "cex/common/replay_kafka.hpp"

namespace cex::backtest::infra {
namespace {

using json = nlohmann::json;

int64_t ToEpochMs(std::chrono::system_clock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             tp.time_since_epoch())
      .count();
}

std::chrono::system_clock::time_point FromEpochMs(const int64_t millis) {
  return std::chrono::system_clock::time_point{std::chrono::milliseconds(millis)};
}

std::optional<std::chrono::system_clock::time_point> ParseDateValue(
    const json& body,
    const char* key) {
  if (!body.contains(key) || body[key].is_null()) return std::nullopt;
  if (body[key].is_number_integer() || body[key].is_number_unsigned()) {
    return FromEpochMs(body[key].get<int64_t>());
  }
  if (!body[key].is_string()) return std::nullopt;
  const std::string value = body[key].get<std::string>();
  if (value.empty()) return std::nullopt;
  if (std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
      })) {
    return FromEpochMs(std::stoll(value));
  }

  const auto parse_utc = [](const std::string& raw,
                            const char* format)
      -> std::optional<std::chrono::system_clock::time_point> {
    std::tm tm{};
    std::istringstream ss(raw);
    ss >> std::get_time(&tm, format);
    if (ss.fail()) return std::nullopt;
#if defined(_WIN32)
    const std::time_t seconds = _mkgmtime(&tm);
#else
    const std::time_t seconds = timegm(&tm);
#endif
    return std::chrono::system_clock::from_time_t(seconds);
  };

  if (value.find('T') != std::string::npos) {
    std::string normalized = value;
    if (!normalized.empty() && normalized.back() == 'Z') {
      normalized.pop_back();
    }
    const auto fractional_pos = normalized.find('.');
    if (fractional_pos != std::string::npos) {
      normalized = normalized.substr(0, fractional_pos);
    }
    std::replace(normalized.begin(), normalized.end(), 'T', ' ');
    if (auto parsed = parse_utc(normalized, "%Y-%m-%d %H:%M:%S");
        parsed.has_value()) {
      return parsed;
    }
  }

  std::tm tm{};
  std::istringstream ss(value);
  ss >> std::get_time(&tm, "%Y-%m-%d");
  if (ss.fail()) return std::nullopt;
#if defined(_WIN32)
  const std::time_t seconds = _mkgmtime(&tm);
#else
  const std::time_t seconds = timegm(&tm);
#endif
  return std::chrono::system_clock::from_time_t(seconds);
}

std::string GetString(const json& body,
                      const std::initializer_list<const char*> keys,
                      const std::string& fallback = "") {
  for (const char* key : keys) {
    if (body.contains(key) && body[key].is_string()) return body[key].get<std::string>();
  }
  return fallback;
}

std::optional<std::string> GetOptionalString(
    const json& body,
    const std::initializer_list<const char*> keys) {
  const std::string value = GetString(body, keys);
  if (value.empty()) return std::nullopt;
  return value;
}

std::optional<int64_t> GetInt64(const json& body,
                                const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (!body.contains(key) || body[key].is_null()) continue;
    if (body[key].is_number_integer() || body[key].is_number_unsigned()) {
      return body[key].get<int64_t>();
    }
    if (body[key].is_string()) return std::stoll(body[key].get<std::string>());
  }
  return std::nullopt;
}

std::optional<double> GetDouble(const json& body,
                                const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (!body.contains(key) || body[key].is_null()) continue;
    if (body[key].is_number()) return body[key].get<double>();
    if (body[key].is_string()) return std::stod(body[key].get<std::string>());
  }
  return std::nullopt;
}

std::optional<std::string> DumpObject(const json& body,
                                      const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (body.contains(key) && !body[key].is_null()) return body[key].dump();
  }
  return std::nullopt;
}

std::optional<std::string> FirstInstrument(const json& body) {
  if (body.contains("instrument") && body["instrument"].is_string()) {
    return body["instrument"].get<std::string>();
  }
  if (!body.contains("instruments") || !body["instruments"].is_array()) {
    return std::nullopt;
  }
  if (body["instruments"].empty()) return std::string{};
  if (!body["instruments"][0].is_string()) return std::string{};
  return body["instruments"][0].get<std::string>();
}

std::optional<app::CreateReplaySession::Request> BuildCreateRequest(
    const cex::common::ReplayCommandMessage& msg,
    const json& body,
    std::string* error) {
  auto from = ParseDateValue(body, "daterangefrom");
  auto to = ParseDateValue(body, "daterangeto");
  if (!from.has_value() || !to.has_value()) {
    *error = "daterangefrom and daterangeto are required";
    return std::nullopt;
  }
  if (!body.contains("strategy")) {
    *error = "strategy is required";
    return std::nullopt;
  }

  app::CreateReplaySession::Request req;
  req.session_id = GetString(body, {"sessionid", "session_id"}, msg.session_id);
  req.user_id = GetString(body, {"userid", "user_id"}, msg.requested_by);
  req.name = GetString(body, {"name"}, "Replay session");
  req.date_range_from = *from;
  req.date_range_to = *to;
  req.strategy_json = body["strategy"].dump();
  req.instrument = FirstInstrument(body);
  req.solver_config_id = GetString(body, {"solverconfigid", "solver_config_id"});
  req.risk_limits_id = GetString(body, {"risklimitsid", "risk_limits_id"});
  req.solver_config_inline_json = DumpObject(body, {"solverconfig", "solver_config"});
  req.risk_limits_inline_json = DumpObject(body, {"risklimits", "risk_limits"});

  req.fee_model_id = GetOptionalString(body, {"feemodelid", "fee_model_id"});
  const bool uses_production_fee =
      (body.contains("feemodel") && body["feemodel"].is_object() &&
       body["feemodel"].value("production", false)) ||
      (body.contains("fee_model") && body["fee_model"].is_object() &&
       body["fee_model"].value("production", false));
  if (uses_production_fee) {
    if (!req.fee_model_id.has_value() || req.fee_model_id->empty()) {
      req.fee_model_id = "production";
    }
  } else {
    req.fee_model_inline_json = DumpObject(body, {"feemodel", "fee_model"});
  }

  for (const char* key : {"rewardmode", "reward_mode"}) {
    if (body.contains(key) && !body[key].is_null() && !body[key].is_string()) {
      *error = std::string(key) + " must be a string";
      return std::nullopt;
    }
  }
  req.reward_mode = GetString(body, {"rewardmode", "reward_mode"}, "incrementalPnL");
  req.reward_config_id = GetOptionalString(body, {"rewardconfigid", "reward_config_id"});
  req.reward_config_inline_json = DumpObject(body, {"rewardconfig", "reward_config"});
  req.random_seed = GetInt64(body, {"randomseed", "random_seed"});
  req.tolerance = GetDouble(body, {"tolerance"});
  // F15-PERSIST: absent key ⇒ true (backward-compat; existing tests omit field).
  req.persist = body.value("persist", true);

  if (body.contains("sessionconfigsnapshot") && body["sessionconfigsnapshot"].is_object()) {
    const auto& snap = body["sessionconfigsnapshot"];
    if (!req.random_seed.has_value()) {
      req.random_seed = GetInt64(snap, {"randomseed", "random_seed"});
    }
    if (!req.tolerance.has_value()) {
      req.tolerance = GetDouble(snap, {"tolerance"});
    }
  }

  return req;
}

app::ReplayConfigRequest ToConfigRequest(
    const app::CreateReplaySession::Request& req) {
  app::ReplayConfigRequest config;
  config.solver_config_id =
      req.solver_config_id.empty() ? "inline" : req.solver_config_id;
  config.risk_limits_id =
      req.risk_limits_id.empty() ? "inline" : req.risk_limits_id;
  config.fee_model_id = req.fee_model_id.value_or(
      req.fee_model_inline_json.has_value() ? "inline" : "production");
  config.reward_config_id = req.reward_config_id.value_or("");
  config.solver_config_inline_override = req.solver_config_inline_json;
  config.risk_limits_inline_override = req.risk_limits_inline_json;
  config.fee_model_inline_override = req.fee_model_inline_json;
  config.reward_config_inline_override = req.reward_config_inline_json;
  if (req.reward_mode.has_value() && !req.reward_mode->empty()) {
    json reward = config.reward_config_inline_override.has_value()
                      ? json::parse(*config.reward_config_inline_override)
                      : json::object();
    reward["mode"] = *req.reward_mode;
    config.reward_config_inline_override = reward.dump();
  }
  config.random_seed = req.random_seed;
  config.tolerance = req.tolerance;
  return config;
}

app::ReplayConfigRequest ToConfigRequest(const app::ReplaySession& session) {
  app::ReplayConfigRequest config;
  config.solver_config_id =
      session.solver_config_id.empty() ? "production" : session.solver_config_id;
  config.risk_limits_id =
      session.risk_limits_id.empty() ? "production" : session.risk_limits_id;
  config.fee_model_id = "production";
  if (!session.fee_model_json.empty()) {
    config.fee_model_inline_override = session.fee_model_json;
    config.fee_model_id = "inline";
  }
  config.reward_config_inline_override = R"({"mode":"incrementalPnL"})";
  return config;
}

void PublishCreateFailure(app::IReplayEventPublisher* publisher,
                          const std::string& session_id,
                          const std::string& error_code,
                          const std::string& error_details) {
  if (publisher == nullptr) return;
  app::ReplayLifecycleEvent evt;
  evt.session_id = session_id;
  evt.status = app::ReplayLifecycleStatus::kFailed;
  evt.error_code = error_code;
  evt.error_details = error_details;
  publisher->PublishLifecycle(evt);
}

}  // namespace

ReplayCommandsKafkaConsumer::ReplayCommandsKafkaConsumer(UseCases uc,
                                                         const std::string& brokers,
                                                         std::string topic)
    : uc_(uc), brokers_(brokers), topic_(std::move(topic)) {}

void ReplayCommandsKafkaConsumer::start() {
  running_.store(true);
  t_ = std::thread([this] { loop(); });
}

void ReplayCommandsKafkaConsumer::stop() {
  running_.store(false);
  if (t_.joinable()) t_.join();
}

void ReplayCommandsKafkaConsumer::loop() {
  cex::common::KafkaConsumer consumer({
      .brokers = brokers_,
      .group_id = "backtest-replay-commands",
      .client_id = "backtest-replay-commands",
      .enable_auto_commit = false,
  });
  consumer.subscribe({topic_});

  cex::common::log_json("INFO", "ReplayCommandsKafkaConsumer started",
                        {{"topic", topic_}});

  while (running_.load()) {
    bool ok = consumer.poll_once(500, [this](const std::string& /*topic*/,
                                             const std::string& /*key*/,
                                             const std::string& payload) {
      cex::common::ReplayCommandMessage msg;
      if (!cex::common::ParseReplayCommandMessage(payload, &msg)) {
        cex::common::log_json("ERROR", "ReplayCommandsConsumer: failed to parse message");
        return;
      }

      switch (msg.command_type) {
        case cex::common::ReplayCommandType::kStart:
          if (uc_.create == nullptr || uc_.run == nullptr) {
            PublishCreateFailure(uc_.events, msg.session_id, "dependency_error",
                                 "Replay start use cases are not configured");
            break;
          }
          try {
            const auto body = json::parse(msg.payload_json);
            std::string error;
            auto create_req = BuildCreateRequest(msg, body, &error);
            if (!create_req.has_value()) {
              PublishCreateFailure(uc_.events, msg.session_id, "validation_error", error);
              break;
            }
            const auto create_result = uc_.create->Run(*create_req);
            if (!create_result.ok) {
              PublishCreateFailure(uc_.events, msg.session_id, create_result.error_code,
                                   create_result.error_message);
              break;
            }

            app::RunReplaySession::Request run_req;
            run_req.session_id = create_result.created.session_id;
            run_req.tracked_user_id = create_result.created.user_id;
            run_req.config_request = ToConfigRequest(*create_req);
            run_req.history_config.from_ms = ToEpochMs(create_req->date_range_from);
            run_req.history_config.to_ms = ToEpochMs(create_req->date_range_to);
            std::thread([runner = uc_.run,
                          reg = uc_.ephemeral_registry,
                          run_req]() mutable {
              runner->Run(run_req);
              // F15-PERSIST: evict ephemeral sessions after Run() returns so
              // they don't accumulate in the registry after reaching terminal
              // state. EvictIfTerminal is a no-op for non-ephemeral ids.
              if (reg != nullptr) {
                reg->EvictIfTerminal(run_req.session_id);
              }
            }).detach();
          } catch (const std::exception& ex) {
            PublishCreateFailure(uc_.events, msg.session_id, "invalid_json", ex.what());
          }
          break;
        case cex::common::ReplayCommandType::kCancel:
          if (uc_.cancel) {
            app::CancelReplaySession::Request req;
            req.session_id = msg.session_id;
            if (!msg.requested_by.empty()) req.requested_by = msg.requested_by;
            const auto result = uc_.cancel->Run(req);
            if (!result.ok) {
              cex::common::log_json("WARN", "CancelReplaySession failed",
                                    {{"session_id", msg.session_id},
                                     {"error_code", result.error_code},
                                     {"error_message", result.error_message}});
            }
            // A pending ephemeral session cancelled before its run thread
            // starts never passes through the run-thread eviction, so evict
            // here. No-op for persisted ids and for non-terminal sessions.
            if (uc_.ephemeral_registry != nullptr) {
              uc_.ephemeral_registry->EvictIfTerminal(msg.session_id);
            }
          }
          break;
        case cex::common::ReplayCommandType::kRetry:
          if (uc_.retry) {
            json body = json::object();
            if (!msg.payload_json.empty()) {
              body = json::parse(msg.payload_json, nullptr, false);
              if (body.is_discarded() || !body.is_object()) {
                PublishCreateFailure(uc_.events, msg.session_id, "invalid_json",
                                     "Invalid retry command JSON");
                break;
              }
            }
            app::RetryReplaySessionRequest req;
            req.original_session_id =
                GetString(body, {"originalsessionid", "original_session_id"},
                          msg.session_id);
            req.user_id = GetString(body, {"userid", "user_id"}, msg.requested_by);
            const auto requested_retry_id =
                GetString(body, {"newsessionid", "new_session_id"});
            if (!requested_retry_id.empty()) req.new_session_id = requested_retry_id;
            const std::string retry_event_session_id =
                req.new_session_id.value_or(msg.session_id);
            const auto result = uc_.retry->Execute(req);
            if (!result.ok) {
              cex::common::log_json("WARN", "RetryReplaySession failed",
                                    {{"session_id", msg.session_id},
                                     {"error_code", result.error_code},
                                     {"error_message", result.error_message}});
              PublishCreateFailure(uc_.events, retry_event_session_id, result.error_code,
                                   result.error_message);
            } else if (uc_.run != nullptr && result.created_session.has_value()) {
              app::RunReplaySession::Request run_req;
              run_req.session_id = result.new_session_id;
              run_req.tracked_user_id = result.created_session->user_id;
              run_req.config_request = ToConfigRequest(*result.created_session);
              run_req.history_config.from_ms =
                  ToEpochMs(result.created_session->date_range_from);
              run_req.history_config.to_ms =
                  ToEpochMs(result.created_session->date_range_to);
              std::thread([runner = uc_.run,
                           reg = uc_.ephemeral_registry,
                           run_req]() mutable {
                runner->Run(run_req);
                // Evict ephemeral retry sessions once terminal so the
                // in-memory registry does not grow unbounded. No-op for
                // persisted ids.
                if (reg != nullptr) {
                  reg->EvictIfTerminal(run_req.session_id);
                }
              }).detach();
            }
          }
          break;
        default:
          cex::common::log_json("DEBUG", "ReplayCommandsConsumer: unhandled command type",
                                {{"command_type",
                                  cex::common::ReplayCommandTypeToString(msg.command_type)},
                                 {"session_id", msg.session_id}});
          break;
      }
    });

    if (!ok) break;
  }

  cex::common::log_json("INFO", "ReplayCommandsKafkaConsumer stopped");
}

}  // namespace cex::backtest::infra
