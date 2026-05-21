#include "infra/http/retry_session_handler.hpp"

#include "crow.h"
#include "cex/common/log.hpp"
#include <nlohmann/json.hpp>

namespace cex::backtest::infra {

using json = nlohmann::json;

RetrySessionHandler::RetrySessionHandler(app::RetryReplaySessionUseCase* retry_uc)
    : retry_uc_(retry_uc) {
  if (!retry_uc_) {
    throw std::invalid_argument("RetrySessionHandler requires a valid RetryReplaySessionUseCase");
  }
}

crow::response RetrySessionHandler::HandleRetryRequest(const crow::request& req) {
  try {
    auto body_json = json::parse(req.body);

    // Validate required fields
    if (!body_json.contains("original_session_id") || !body_json.contains("user_id")) {
      return crow::response(400, json{
        {"ok", false},
        {"error_code", "invalid_request"},
        {"error_message", "Missing required fields: original_session_id, user_id"}
      }.dump());
    }

    app::RetryReplaySessionRequest request{
        .original_session_id = body_json["original_session_id"].get<std::string>(),
        .user_id = body_json["user_id"].get<std::string>()
    };

    const auto response = retry_uc_->Execute(request);

    if (response.ok) {
      cex::common::log_json("INFO", "Retry session created successfully",
                            {{"original_session_id", request.original_session_id},
                             {"new_session_id", response.new_session_id},
                             {"user_id", request.user_id}});

      return crow::response(200, json{
        {"ok", true},
        {"new_session_id", response.new_session_id}
      }.dump());
    } else {
      cex::common::log_json("WARN", "Failed to create retry session",
                            {{"error_code", response.error_code},
                             {"error_message", response.error_message},
                             {"original_session_id", request.original_session_id}});

      int http_code = 500;
      if (response.error_code == "not_found") {
        http_code = 404;
      } else if (response.error_code == "permission_denied") {
        http_code = 403;
      } else if (response.error_code == "invalid_state") {
        http_code = 409;
      }

      return crow::response(http_code, json{
        {"ok", false},
        {"error_code", response.error_code},
        {"error_message", response.error_message}
      }.dump());
    }

  } catch (const json::exception& ex) {
    cex::common::log_json("ERROR", "JSON parsing error in retry request",
                          {{"error", std::string(ex.what())}});
    return crow::response(400, json{
      {"ok", false},
      {"error_code", "invalid_json"},
      {"error_message", std::string(ex.what())}
    }.dump());
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Unexpected error in retry handler",
                          {{"error", std::string(ex.what())}});
    return crow::response(500, json{
      {"ok", false},
      {"error_code", "internal_error"},
      {"error_message", "Internal server error"}
    }.dump());
  }
}

}  // namespace cex::backtest::infra
