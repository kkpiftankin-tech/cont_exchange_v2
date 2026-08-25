#include "infra/replay_results_kafka_consumer.hpp"

#include "cex/common/log.hpp"
#include "cex/common/replay_kafka.hpp"

namespace cex::gateway::infra {

ReplayResultsKafkaConsumer::ReplayResultsKafkaConsumer(ReplayResultsHub* hub,
                                                       std::string brokers,
                                                       std::string topic)
    : hub_(hub), brokers_(std::move(brokers)), topic_(std::move(topic)) {}

void ReplayResultsKafkaConsumer::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { loop(); });
}

void ReplayResultsKafkaConsumer::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void ReplayResultsKafkaConsumer::loop() {
  cex::common::KafkaConsumer consumer({
      .brokers = brokers_,
      .group_id = "gateway-replay-results",
      .client_id = "gateway-replay-results",
      .enable_auto_commit = false,
  });
  consumer.subscribe({topic_});

  cex::common::log_json("INFO", "Gateway replay.results consumer started",
                        {{"topic", topic_}});

  while (running_.load()) {
    const bool ok = consumer.poll_once(500, [this](const std::string& topic,
                                                   const std::string& key,
                                                   const std::string& payload) {
      (void)topic;
      (void)key;
      if (hub_ == nullptr) return;
      cex::common::ReplayResultMessage msg;
      if (!cex::common::ParseReplayResultMessage(payload, &msg)) {
        cex::common::log_json("ERROR", "Gateway failed to parse replay.results payload");
        return;
      }
      hub_->Publish(msg);
    });
    if (!ok) break;
  }

  cex::common::log_json("INFO", "Gateway replay.results consumer stopped",
                        {{"topic", topic_}});
}

}  // namespace cex::gateway::infra
