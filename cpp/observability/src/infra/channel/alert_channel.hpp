#pragma once

#include <boost/thread/sync_bounded_queue.hpp>

#include "domain/ports/i_alert_publisher.hpp"
#include "domain/ports/i_alert_subscriber.hpp"

namespace cex::observability::infra {

class AlertChannel : public domain::IAlertPublisher, public domain::IAlertSubscriber {
 public:
  explicit AlertChannel(std::size_t capacity = 64) : alert_queue_(capacity) {}

  void Publish(const domain::Alert& alert) override { alert_queue_.push(alert); }

  std::optional<domain::Alert> Consume() override {
    domain::Alert alert;
    if (alert_queue_.wait_pull_front(alert) == boost::concurrent::queue_op_status::closed) {
      return std::nullopt;
    }
    return alert;
  }

  void Close() { alert_queue_.close(); }

 private:
  boost::sync_bounded_queue<domain::Alert> alert_queue_;
};

}  // namespace cex::observability::infra
