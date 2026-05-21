#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "app/replay_orchestration_ports.hpp"
#include "cex/common/replay_kafka.hpp"
#include "infra/kafka_replay_event_publisher.hpp"

namespace {

using cex::backtest::app::ReplayLifecycleEvent;
using cex::backtest::app::ReplayLifecycleStatus;
using cex::backtest::app::ReplayProgressEvent;
using cex::backtest::app::ReplaySummary;
using cex::backtest::infra::KafkaReplayEventPublisher;
using cex::common::ParseReplayResultMessage;
using cex::common::ReplayResultKind;
using cex::common::ReplayResultMessage;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool test_progress_event_serialization() {
  struct Sent {
    std::string topic;
    std::string key;
    std::string payload;
  };
  std::vector<Sent> sent;
  KafkaReplayEventPublisher publisher(
      [&sent](const std::string& topic, const std::string& key, const std::string& payload) {
        sent.push_back({topic, key, payload});
        return true;
      },
      "replay.results");

  publisher.PublishProgress({"sess-1", 7, 10});
  if (!Check(sent.size() == 1, "one progress message published")) return false;
  if (!Check(sent[0].topic == "replay.results", "topic set")) return false;
  if (!Check(sent[0].key == "sess-1", "session key used")) return false;

  ReplayResultMessage parsed;
  if (!Check(ParseReplayResultMessage(sent[0].payload, &parsed), "payload parseable")) return false;
  if (!Check(parsed.kind == ReplayResultKind::kProgress, "kind=progress")) return false;
  if (!Check(parsed.session_id == "sess-1", "session propagated")) return false;
  if (!Check(parsed.batch_seq == 7 && parsed.total_batches == 10, "progress numbers")) return false;
  return true;
}

bool test_lifecycle_summary_serialization() {
  std::vector<std::string> payloads;
  KafkaReplayEventPublisher publisher(
      [&payloads](const std::string&, const std::string&, const std::string& payload) {
        payloads.push_back(payload);
        return true;
      },
      "replay.results");

  ReplaySummary summary;
  summary.session_id = "sess-2";
  summary.total_batches = 100;
  summary.processed_batches = 91;
  summary.failed_batches = 2;
  summary.partial = true;
  summary.total_pnl = 42.5;
  summary.avg_pnl = 0.5;
  summary.avg_is = -1.2;
  summary.sharpe = 1.75;
  summary.avg_fill_rate = 0.91;
  summary.avg_solve_time_ms = 12.0;
  summary.max_drawdown = 3.2;

  ReplayLifecycleEvent evt;
  evt.session_id = "sess-2";
  evt.status = ReplayLifecycleStatus::kCompleted;
  evt.summary = summary;
  evt.error_details = std::string{"soft issues"};
  publisher.PublishLifecycle(evt);

  if (!Check(payloads.size() == 1, "one lifecycle message published")) return false;
  ReplayResultMessage parsed;
  if (!Check(ParseReplayResultMessage(payloads[0], &parsed), "lifecycle payload parseable")) {
    return false;
  }
  if (!Check(parsed.kind == ReplayResultKind::kLifecycle, "kind=lifecycle")) return false;
  if (!Check(parsed.status == "completed", "status propagated")) return false;
  if (!Check(parsed.has_summary, "summary propagated")) return false;
  if (!Check(parsed.total_batches == 100 && parsed.processed_batches == 91, "summary counts")) {
    return false;
  }
  if (!Check(std::abs(parsed.sharpe - 1.75) < 1e-9, "sharpe propagated")) return false;
  if (!Check(parsed.error_details == "soft issues", "error details propagated")) return false;
  return true;
}

// F15-BACKTEST-8: extra summary fields (std_pnl, avg_vwap) must round-trip
// from ReplayLifecycleEvent through KafkaReplayEventPublisher and the
// payload parser, so downstream consumers can render the full ReplaySummary
// without re-querying PostgreSQL.
bool test_lifecycle_carries_std_pnl_and_avg_vwap() {
  std::vector<std::string> payloads;
  KafkaReplayEventPublisher publisher(
      [&payloads](const std::string&, const std::string&, const std::string& payload) {
        payloads.push_back(payload);
        return true;
      },
      "replay.results");

  ReplaySummary summary;
  summary.session_id = "sess-3";
  summary.total_batches = 5;
  summary.processed_batches = 5;
  summary.failed_batches = 0;
  summary.partial = false;
  summary.std_pnl = 4.5;
  summary.avg_vwap = 100.5;

  ReplayLifecycleEvent evt;
  evt.session_id = "sess-3";
  evt.status = ReplayLifecycleStatus::kCompleted;
  evt.summary = summary;
  publisher.PublishLifecycle(evt);

  if (!Check(payloads.size() == 1, "one lifecycle message published")) return false;
  ReplayResultMessage parsed;
  if (!Check(ParseReplayResultMessage(payloads[0], &parsed), "payload parseable")) return false;
  if (!Check(parsed.has_summary, "summary propagated")) return false;
  if (!Check(std::abs(parsed.std_pnl - 4.5) < 1e-9, "std_pnl propagated")) return false;
  if (!Check(std::abs(parsed.avg_vwap - 100.5) < 1e-9, "avg_vwap propagated")) return false;
  return true;
}

// F15-BACKTEST-8: failed lifecycle carries both error_details and the
// machine-readable error_code so consumers can switch on the code.
bool test_failed_lifecycle_publishes_error_code() {
  std::vector<std::string> payloads;
  KafkaReplayEventPublisher publisher(
      [&payloads](const std::string&, const std::string&, const std::string& payload) {
        payloads.push_back(payload);
        return true;
      },
      "replay.results");

  ReplayLifecycleEvent evt;
  evt.session_id = "sess-fail";
  evt.status = ReplayLifecycleStatus::kFailed;
  evt.error_details = std::string{"solver diverged"};
  evt.error_code = std::string{"solver_error"};
  publisher.PublishLifecycle(evt);

  if (!Check(payloads.size() == 1, "one lifecycle message published")) return false;
  ReplayResultMessage parsed;
  if (!Check(ParseReplayResultMessage(payloads[0], &parsed), "payload parseable")) return false;
  if (!Check(parsed.status == "failed", "status=failed")) return false;
  if (!Check(parsed.error_details == "solver diverged", "error_details propagated")) return false;
  if (!Check(parsed.error_code == "solver_error", "error_code propagated")) return false;
  if (!Check(!parsed.has_summary, "no summary on init failure")) return false;
  return true;
}

// F15-BACKTEST-8: cancelled lifecycle event reaches the topic with status
// "cancelled" and the partial summary (if any) attached.
bool test_cancelled_lifecycle_event() {
  std::vector<std::string> payloads;
  KafkaReplayEventPublisher publisher(
      [&payloads](const std::string&, const std::string&, const std::string& payload) {
        payloads.push_back(payload);
        return true;
      },
      "replay.results");

  ReplaySummary summary;
  summary.session_id = "sess-cancel";
  summary.total_batches = 10;
  summary.processed_batches = 4;
  summary.partial = true;

  ReplayLifecycleEvent evt;
  evt.session_id = "sess-cancel";
  evt.status = ReplayLifecycleStatus::kCancelled;
  evt.summary = summary;
  publisher.PublishLifecycle(evt);

  ReplayResultMessage parsed;
  if (!Check(ParseReplayResultMessage(payloads.at(0), &parsed), "payload parseable")) return false;
  if (!Check(parsed.status == "cancelled", "status=cancelled")) return false;
  if (!Check(parsed.has_summary && parsed.partial,
             "partial summary propagated on cancel")) return false;
  if (!Check(parsed.processed_batches == 4 && parsed.total_batches == 10,
             "cancel snapshot counts")) return false;
  return true;
}

// F15-BACKTEST-8: progress events must be keyed by session_id so the
// topic partitions preserve per-session ordering.
bool test_progress_uses_session_as_kafka_key() {
  std::vector<std::pair<std::string, std::string>> sent_keys;  // (topic, key)
  KafkaReplayEventPublisher publisher(
      [&sent_keys](const std::string& topic,
                   const std::string& key,
                   const std::string&) {
        sent_keys.emplace_back(topic, key);
        return true;
      },
      "replay.results");

  publisher.PublishProgress({"sess-A", 1, 100});
  publisher.PublishProgress({"sess-A", 2, 100});
  publisher.PublishProgress({"sess-B", 1, 100});

  ReplayLifecycleEvent done;
  done.session_id = "sess-A";
  done.status = ReplayLifecycleStatus::kCompleted;
  publisher.PublishLifecycle(done);

  if (!Check(sent_keys.size() == 4, "all events delivered")) return false;
  for (const auto& [topic, key] : sent_keys) {
    if (!Check(topic == "replay.results", "all events go to replay.results")) return false;
    if (!Check(!key.empty(), "non-empty session key")) return false;
  }
  if (!Check(sent_keys[0].second == "sess-A" && sent_keys[1].second == "sess-A",
             "progress for sess-A keyed by sess-A")) return false;
  if (!Check(sent_keys[2].second == "sess-B", "progress for sess-B keyed by sess-B")) return false;
  if (!Check(sent_keys[3].second == "sess-A", "lifecycle keyed by sess-A")) return false;
  return true;
}

// F15-BACKTEST-8: send-side failures must not throw; the publisher reports
// the failure via metrics (when supplied) but stays usable.
bool test_publisher_tolerates_send_failure() {
  KafkaReplayEventPublisher publisher(
      [](const std::string&, const std::string&, const std::string&) { return false; },
      "replay.results");

  // Should not throw / abort.
  publisher.PublishProgress({"sess-x", 1, 5});
  ReplayLifecycleEvent evt;
  evt.session_id = "sess-x";
  evt.status = ReplayLifecycleStatus::kFailed;
  evt.error_details = std::string{"network down"};
  publisher.PublishLifecycle(evt);
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_progress_event_serialization() && ok;
  ok = test_lifecycle_summary_serialization() && ok;
  ok = test_lifecycle_carries_std_pnl_and_avg_vwap() && ok;
  ok = test_failed_lifecycle_publishes_error_code() && ok;
  ok = test_cancelled_lifecycle_event() && ok;
  ok = test_progress_uses_session_as_kafka_key() && ok;
  ok = test_publisher_tolerates_send_failure() && ok;
  if (ok) {
    std::cout << "[OK] backtest_kafka_replay_event_publisher_test passed" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
