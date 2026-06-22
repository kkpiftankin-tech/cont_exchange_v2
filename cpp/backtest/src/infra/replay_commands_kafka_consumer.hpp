#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "cex/common/kafka.hpp"
#include "app/cancel_replay_session_uc.hpp"
#include "app/create_replay_session_uc.hpp"
#include "app/retry_replay_session_uc.hpp"
#include "app/run_replay_session_uc.hpp"
#include "infra/ephemeral_session_registry.hpp"

namespace cex::backtest::infra {

// Consumes replay.commands Kafka topic and dispatches to use cases.
// Currently handles: kCancel -> CancelReplaySession.
class ReplayCommandsKafkaConsumer {
 public:
  struct UseCases {
    app::CancelReplaySession* cancel{nullptr};
    app::CreateReplaySession* create{nullptr};
    app::RunReplaySession* run{nullptr};
    app::RetryReplaySessionUseCase* retry{nullptr};
    app::IReplayEventPublisher* events{nullptr};
    // F15-PERSIST: optional — if non-null, ephemeral sessions are evicted after
    // the Run use case completes and the session reaches a terminal state.
    EphemeralSessionRegistry* ephemeral_registry{nullptr};
  };

  ReplayCommandsKafkaConsumer(UseCases uc,
                              const std::string& brokers,
                              std::string topic = "replay.commands");

  void start();
  void stop();

 private:
  void loop();

  UseCases uc_;
  std::string brokers_;
  std::string topic_;
  std::atomic<bool> running_{false};
  std::thread t_;
};

}  // namespace cex::backtest::infra
