#pragma once
#include <atomic>
#include <thread>
#include <unordered_map>

#include "cex/common/kafka.hpp"
#include "cex/common/decimal.hpp"
#include "fob/orders/v1/orders.pb.h"

namespace cex::matching::app {

// Simple periodic matching loop (MVP simulator):
// - consumes orders.normalized (create/cancel/amend)
// - every batch_interval_ms runs a "pseudo solver":
//     fill dq = min(remaining_qty, max_speed * dt)
//     execution price = midpoint(price_low, price_high)
// - produces batch.outputs (BatchResult)
class MatchingLoop {
 public:
  MatchingLoop(const std::string& brokers, int batch_interval_ms);

  void start();
  void stop();

 private:
  void consume_orders_loop();
  void batch_timer_loop();

  void on_order_event(const fob::orders::v1::OrdersNormalized& evt);
  void run_one_batch();

  std::string brokers_;
  int batch_interval_ms_;

  cex::common::KafkaProducer producer_;
  cex::common::KafkaConsumer consumer_;

  std::atomic<bool> running_{false};
  std::thread t_consume_;
  std::thread t_batch_;

  // Active orders by order_id
  std::unordered_map<std::string, fob::orders::v1::FlowOrder> active_;
};

}  // namespace cex::matching::app
