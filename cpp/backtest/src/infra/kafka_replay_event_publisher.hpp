#pragma once

#include <functional>
#include <string>

#include "app/replay_orchestration_ports.hpp"
#include "app/replay_runtime_metrics.hpp"
#include "cex/common/kafka.hpp"

namespace cex::backtest::infra {

class KafkaReplayEventPublisher final : public app::IReplayEventPublisher {
 public:
  using SendFn = std::function<bool(const std::string& topic,
                                    const std::string& key,
                                    const std::string& payload)>;

  explicit KafkaReplayEventPublisher(cex::common::KafkaProducer producer,
                                     std::string topic = "replay.results",
                                     app::ReplayRuntimeMetrics* metrics = nullptr);
  explicit KafkaReplayEventPublisher(SendFn send,
                                     std::string topic = "replay.results",
                                     app::ReplayRuntimeMetrics* metrics = nullptr);

  void PublishProgress(const app::ReplayProgressEvent& evt) override;
  void PublishLifecycle(const app::ReplayLifecycleEvent& evt) override;

 private:
  SendFn send_;
  std::string topic_;
  app::ReplayRuntimeMetrics* metrics_{nullptr};
};

}  // namespace cex::backtest::infra
