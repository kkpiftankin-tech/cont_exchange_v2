#include "infra/positions_update_kafka_consumer.hpp"

#include <utility>

#include <nlohmann/json.hpp>

#include "cex/common/kafka.hpp"
#include "cex/common/log.hpp"

namespace cex::gateway::infra {

namespace {
using json = nlohmann::json;

// Лёгкий парсер сигнала {user_id, batch_id, ts}. ts не используется потребителем
// (сигнал транзиентный, snapshot реагрегируется из ledger/risk), поэтому
// достаточно вытащить user_id + batch_id. user_id обязателен; batch_id может
// отсутствовать — тогда дедуп по batch_id невозможен, но снимок всё равно
// перечитываем (используем "" как batch_id).
bool ParsePositionsUpdate(const std::string& payload,
                          std::string* user_id,
                          std::string* batch_id) {
  try {
    const json j = json::parse(payload);
    if (!j.is_object()) return false;
    if (!j.contains("user_id") || !j["user_id"].is_string()) return false;
    *user_id = j["user_id"].get<std::string>();
    if (user_id->empty()) return false;
    if (j.contains("batch_id") && j["batch_id"].is_string()) {
      *batch_id = j["batch_id"].get<std::string>();
    } else {
      batch_id->clear();
    }
    return true;
  } catch (const std::exception&) {
    return false;
  }
}
}  // namespace

PositionsUpdateKafkaConsumer::PositionsUpdateKafkaConsumer(PositionsHub* hub,
                                                           std::string brokers,
                                                           std::string topic)
    : hub_(hub), brokers_(std::move(brokers)), topic_(std::move(topic)) {}

void PositionsUpdateKafkaConsumer::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { loop(); });
}

void PositionsUpdateKafkaConsumer::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

bool PositionsUpdateKafkaConsumer::HandlePayload(const std::string& payload) {
  if (hub_ == nullptr) return false;
  std::string user_id;
  std::string batch_id;
  if (!ParsePositionsUpdate(payload, &user_id, &batch_id)) {
    cex::common::log_json("ERROR",
                          "Gateway failed to parse positions.update payload");
    return false;
  }

  // Идемпотентность по (user_id, batch_id) (ADR-020): один и тот же batch_id
  // для пользователя реагрегируем единожды. Пустой batch_id не дедупим —
  // сигнал без батча всегда триггерит перечитывание.
  if (!batch_id.empty()) {
    const std::string key = user_id + '\x1f' + batch_id;
    if (!seen_keys_.insert(key).second) {
      return false;  // дубликат — не пушим повторно
    }
    seen_order_.push_back(key);
    while (seen_order_.size() > kDedupCapacity) {
      seen_keys_.erase(seen_order_.front());
      seen_order_.pop_front();
    }
  }

  hub_->Publish(user_id, batch_id);
  return true;
}

void PositionsUpdateKafkaConsumer::loop() {
  cex::common::KafkaConsumer consumer({
      .brokers = brokers_,
      .group_id = "gateway-positions",
      .client_id = "gateway-positions",
      .enable_auto_commit = false,
  });
  consumer.subscribe({topic_});

  cex::common::log_json("INFO", "Gateway positions.update consumer started",
                        {{"topic", topic_}});

  while (running_.load()) {
    const bool ok = consumer.poll_once(
        500, [this](const std::string& topic, const std::string& key,
                    const std::string& payload) {
          (void)topic;
          (void)key;
          HandlePayload(payload);
        });
    if (!ok) break;
  }

  cex::common::log_json("INFO", "Gateway positions.update consumer stopped",
                        {{"topic", topic_}});
}

}  // namespace cex::gateway::infra
