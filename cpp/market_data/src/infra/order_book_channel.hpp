#pragma once

#include <optional>
#include <queue>

#include <boost/thread/sync_bounded_queue.hpp>

#include "domain/ports/i_order_book_publisher.hpp"
#include "domain/ports/i_order_book_subscriber.hpp"
#include "fob/marketdata/v1/bbo.pb.h"

namespace cex::market_data::infra {

// Structure for BBO message in the queue
struct BBOWithMeta {
  domain::BBO bbo;
  std::string symbol;
  uint64_t nonce;
  domain::Timestamp timestamp;
};

class OrderBookChannel : public domain::IOrderBookPublisher, public domain::IOrderBookSubscriber {
 public:
  explicit OrderBookChannel(std::size_t capacity = 64) 
      : full_book_queue_(capacity), bbo_queue_(capacity) {}

  void Publish(const domain::OrderBook& book) override {
    if (full_book_queue_.try_push(book)) {
      return;
    }

    domain::OrderBook dropped;
    (void)full_book_queue_.try_pull_front(dropped);
    (void)full_book_queue_.try_push(book);
  }

  void PublishBBO(const domain::BBO& bbo, const std::string& symbol,
                  uint64_t nonce, domain::Timestamp timestamp) override {
    BBOWithMeta payload{bbo, symbol, nonce, timestamp};
    if (bbo_queue_.try_push(payload)) {
      return;
    }

    BBOWithMeta dropped{};
    (void)bbo_queue_.try_pull_front(dropped);
    (void)bbo_queue_.try_push(payload);
  }

  std::optional<domain::OrderBook> Consume() override {
    domain::OrderBook s;
    if (full_book_queue_.wait_pull_front(s) == boost::concurrent::queue_op_status::closed) {
      return std::nullopt;
    }
    return s;
  }

  // New: consume BBO updates
  std::optional<BBOWithMeta> ConsumeBBO() {
    BBOWithMeta bbo;
    if (bbo_queue_.wait_pull_front(bbo) == boost::concurrent::queue_op_status::closed) {
      return std::nullopt;
    }
    return bbo;
  }

  void Close() { 
    full_book_queue_.close();
    bbo_queue_.close();
  }

 private:
  boost::sync_bounded_queue<domain::OrderBook> full_book_queue_;
  boost::sync_bounded_queue<BBOWithMeta> bbo_queue_;
};

}  // namespace cex::market_data::infra
