#pragma once

#include "crow.h"
#include "app/retry_replay_session_uc.hpp"

namespace cex::backtest::infra {

class RetrySessionHandler {
 public:
  explicit RetrySessionHandler(app::RetryReplaySessionUseCase* retry_uc);

  crow::response HandleRetryRequest(const crow::request& req);

 private:
  app::RetryReplaySessionUseCase* retry_uc_{nullptr};
};

}  // namespace cex::backtest::infra
