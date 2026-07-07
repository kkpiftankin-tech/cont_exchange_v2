#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "infra/positions_hub.hpp"
#include "infra/positions_update_kafka_consumer.hpp"

namespace {

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool test_subscribe_and_filter() {
  cex::gateway::infra::PositionsHub hub;
  std::vector<std::string> seen;
  const uint64_t id = hub.Subscribe(
      [&](const cex::gateway::infra::PositionsHub::Event& e) {
        seen.push_back(e.user_id + ":" + e.batch_id);
      });

  hub.Publish("user-a", "batch-1");
  hub.Publish("user-b", "batch-2");

  if (!Check(seen.size() == 2, "subscriber receives all events")) return false;
  if (!Check(seen[0] == "user-a:batch-1", "first event payload")) return false;
  if (!Check(seen[1] == "user-b:batch-2", "second event payload")) return false;

  hub.Unsubscribe(id);
  hub.Publish("user-a", "batch-3");
  return Check(seen.size() == 2, "unsubscribed listener is not called");
}

bool test_throwing_listener_isolated() {
  cex::gateway::infra::PositionsHub hub;
  int healthy = 0;
  hub.Subscribe([](const cex::gateway::infra::PositionsHub::Event&) {
    throw std::runtime_error("boom");
  });
  hub.Subscribe([&](const cex::gateway::infra::PositionsHub::Event&) {
    ++healthy;
  });
  hub.Publish("user-a", "batch-1");
  return Check(healthy == 1,
               "healthy subscriber still notified after a listener throws");
}

bool test_consumer_parse_and_dedup() {
  cex::gateway::infra::PositionsHub hub;
  int published = 0;
  hub.Subscribe([&](const cex::gateway::infra::PositionsHub::Event&) {
    ++published;
  });

  cex::gateway::infra::PositionsUpdateKafkaConsumer consumer(&hub, "unused");

  // Valid signal → pushed once.
  if (!Check(consumer.HandlePayload(
                 R"({"user_id":"u1","batch_id":"b1","ts":1})"),
             "valid payload accepted")) {
    return false;
  }
  // Duplicate (same user_id + batch_id) → not pushed again.
  if (!Check(!consumer.HandlePayload(
                 R"({"user_id":"u1","batch_id":"b1","ts":2})"),
             "duplicate batch_id deduped")) {
    return false;
  }
  // Same batch_id but different user → distinct, pushed.
  if (!Check(consumer.HandlePayload(
                 R"({"user_id":"u2","batch_id":"b1"})"),
             "same batch_id different user is distinct")) {
    return false;
  }
  // Different batch_id, same user → pushed.
  if (!Check(consumer.HandlePayload(
                 R"({"user_id":"u1","batch_id":"b2"})"),
             "new batch_id accepted")) {
    return false;
  }
  // Invalid JSON → rejected.
  if (!Check(!consumer.HandlePayload("not-json"), "invalid json rejected")) {
    return false;
  }
  // Missing user_id → rejected.
  if (!Check(!consumer.HandlePayload(R"({"batch_id":"b9"})"),
             "missing user_id rejected")) {
    return false;
  }

  return Check(published == 3, "exactly 3 distinct signals pushed to hub");
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_subscribe_and_filter() && ok;
  ok = test_throwing_listener_isolated() && ok;
  ok = test_consumer_parse_and_dedup() && ok;
  if (ok) {
    std::cout << "[OK] gateway_positions_hub_test passed" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
