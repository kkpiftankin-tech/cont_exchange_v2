#include "infra/kafka_replay_event_publisher.hpp"

#include <chrono>
#include <memory>
#include <utility>

#include "cex/common/log.hpp"
#include "cex/common/replay_kafka.hpp"

namespace cex::backtest::infra {
namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string LifecycleStatusToString(app::ReplayLifecycleStatus status) {
  switch (status) {
    case app::ReplayLifecycleStatus::kRunning: return "running";
    case app::ReplayLifecycleStatus::kCompleted: return "completed";
    case app::ReplayLifecycleStatus::kFailed: return "failed";
    case app::ReplayLifecycleStatus::kCancelled: return "cancelled";
  }
  return "running";
}

}  // namespace

KafkaReplayEventPublisher::KafkaReplayEventPublisher(cex::common::KafkaProducer producer,
                                                     std::string topic,
                                                     app::ReplayRuntimeMetrics* metrics)
    : topic_(std::move(topic)), metrics_(metrics) {
  auto shared_producer =
      std::make_shared<cex::common::KafkaProducer>(std::move(producer));
  send_ = [shared_producer](const std::string& topic_name,
                            const std::string& key,
                            const std::string& payload) {
    return shared_producer->produce(topic_name, key, payload);
  };
}

KafkaReplayEventPublisher::KafkaReplayEventPublisher(SendFn send,
                                                     std::string topic,
                                                     app::ReplayRuntimeMetrics* metrics)
    : send_(std::move(send)), topic_(std::move(topic)), metrics_(metrics) {}

void KafkaReplayEventPublisher::PublishProgress(const app::ReplayProgressEvent& evt) {
  if (!send_) return;

  cex::common::ReplayResultMessage msg;
  msg.kind = cex::common::ReplayResultKind::kProgress;
  msg.session_id = evt.session_id;
  msg.published_at_ms = NowMs();
  msg.batch_seq = evt.batch_seq;
  msg.total_batches = evt.total_batches;
  const auto started = std::chrono::steady_clock::now();
  const bool ok = send_(topic_, evt.session_id,
                        cex::common::SerializeReplayResultMessage(msg));
  if (metrics_ != nullptr) {
    const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    metrics_->ObserveKafkaPublish(topic_, static_cast<double>(latency_ms), ok);
  }
  if (ok) {
    cex::common::log_json("INFO", "Published replay progress",
                          {{"topic", topic_},
                           {"session_id", evt.session_id},
                           {"batch_seq", std::to_string(evt.batch_seq)}});
  }
}

void KafkaReplayEventPublisher::PublishLifecycle(const app::ReplayLifecycleEvent& evt) {
  if (!send_) return;

  cex::common::ReplayResultMessage msg;
  msg.kind = cex::common::ReplayResultKind::kLifecycle;
  msg.session_id = evt.session_id;
  msg.published_at_ms = NowMs();
  msg.status = LifecycleStatusToString(evt.status);
  if (evt.summary.has_value()) {
    msg.has_summary = true;
    msg.total_batches = evt.summary->total_batches;
    msg.processed_batches = evt.summary->processed_batches;
    msg.failed_batches = evt.summary->failed_batches;
    msg.total_fill_events = evt.summary->total_fill_events;
    msg.partial = evt.summary->partial;
    msg.total_pnl = evt.summary->total_pnl;
    msg.avg_pnl = evt.summary->avg_pnl;
    msg.avg_is = evt.summary->avg_is;
    msg.sharpe = evt.summary->sharpe;
    msg.avg_fill_rate = evt.summary->avg_fill_rate;
    msg.avg_solve_time_ms = evt.summary->avg_solve_time_ms;
    msg.max_drawdown = evt.summary->max_drawdown;
    msg.std_pnl = evt.summary->std_pnl;
    msg.avg_vwap = evt.summary->avg_vwap;
    msg.avgis_rule = evt.summary->avgis_rule;
    msg.decision_price_source = evt.summary->decision_price_source;
    msg.no_requested_volume = evt.summary->no_requested_volume;
  }
  if (evt.error_details.has_value()) msg.error_details = *evt.error_details;
  if (evt.error_code.has_value()) msg.error_code = *evt.error_code;

  const auto started = std::chrono::steady_clock::now();
  const bool ok = send_(topic_, evt.session_id,
                        cex::common::SerializeReplayResultMessage(msg));
  if (metrics_ != nullptr) {
    const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    metrics_->ObserveKafkaPublish(topic_, static_cast<double>(latency_ms), ok);
  }
  if (ok) {
    cex::common::log_json("INFO", "Published replay lifecycle",
                          {{"topic", topic_},
                           {"session_id", evt.session_id},
                           {"status", msg.status}});
  }
}

}  // namespace cex::backtest::infra
