#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "cex/common/replay_kafka.hpp"
#include "infra/replay_results_hub.hpp"

namespace {

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool test_latest_and_wait_next() {
  cex::gateway::infra::ReplayResultsHub hub;
  cex::common::ReplayResultMessage progress;
  progress.kind = cex::common::ReplayResultKind::kProgress;
  progress.session_id = "sess-1";
  progress.batch_seq = 2;
  progress.total_batches = 5;
  hub.Publish(progress);

  const auto latest = hub.Latest("sess-1");
  if (!Check(latest.has_value(), "latest exists")) return false;
  if (!Check(latest->batch_seq == 2, "latest batch seq")) return false;

  cex::gateway::infra::ReplayResultsHub::EventEnvelope envelope;
  if (!Check(hub.WaitNext("sess-1", 0, std::chrono::milliseconds(1), &envelope),
             "wait next returns first event")) {
    return false;
  }
  if (!Check(envelope.sequence == 1, "sequence starts at 1")) return false;
  if (!Check(envelope.message.batch_seq == 2, "event payload preserved")) return false;
  return true;
}

bool test_session_filtering() {
  cex::gateway::infra::ReplayResultsHub hub;
  cex::common::ReplayResultMessage event_a;
  event_a.kind = cex::common::ReplayResultKind::kLifecycle;
  event_a.session_id = "A";
  event_a.status = "running";
  hub.Publish(event_a);

  cex::common::ReplayResultMessage event_b;
  event_b.kind = cex::common::ReplayResultKind::kLifecycle;
  event_b.session_id = "B";
  event_b.status = "completed";
  hub.Publish(event_b);

  cex::gateway::infra::ReplayResultsHub::EventEnvelope envelope;
  if (!Check(hub.WaitNext("B", 0, std::chrono::milliseconds(1), &envelope),
             "filter by session id")) {
    return false;
  }
  return Check(envelope.message.session_id == "B", "matched requested session");
}

bool test_subscribe_and_unsubscribe() {
  cex::gateway::infra::ReplayResultsHub hub;
  int callback_count = 0;
  std::string last_session;
  const uint64_t listener_id = hub.Subscribe(
      [&](const cex::gateway::infra::ReplayResultsHub::EventEnvelope& envelope) {
        ++callback_count;
        last_session = envelope.message.session_id;
      });

  cex::common::ReplayResultMessage event;
  event.kind = cex::common::ReplayResultKind::kProgress;
  event.session_id = "sess-ws";
  hub.Publish(event);

  if (!Check(callback_count == 1, "subscriber receives published event")) return false;
  if (!Check(last_session == "sess-ws", "subscriber receives event payload")) return false;

  hub.Unsubscribe(listener_id);
  event.session_id = "sess-after-unsubscribe";
  hub.Publish(event);
  return Check(callback_count == 1, "unsubscribed listener is not called");
}

bool test_throwing_listener_does_not_block_other_subscribers() {
  cex::gateway::infra::ReplayResultsHub hub;
  int healthy_count = 0;
  hub.Subscribe([](const cex::gateway::infra::ReplayResultsHub::EventEnvelope&) {
    throw std::runtime_error("listener failed");
  });
  hub.Subscribe([&](const cex::gateway::infra::ReplayResultsHub::EventEnvelope&) {
    ++healthy_count;
  });

  cex::common::ReplayResultMessage event;
  event.kind = cex::common::ReplayResultKind::kLifecycle;
  event.session_id = "sess-ws";
  event.status = "completed";
  hub.Publish(event);

  return Check(healthy_count == 1,
               "healthy subscriber receives event after another listener throws");
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_latest_and_wait_next() && ok;
  ok = test_session_filtering() && ok;
  ok = test_subscribe_and_unsubscribe() && ok;
  ok = test_throwing_listener_does_not_block_other_subscribers() && ok;
  if (ok) {
    std::cout << "[OK] gateway_replay_results_hub_test passed" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
