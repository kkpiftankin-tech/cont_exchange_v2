#pragma once

#include "domain/entities/order_book.hpp"

namespace cex::market_data::domain {

struct IOrderBookSubscriber {
  virtual std::optional<OrderBook> Consume() = 0;

 protected:
  virtual ~IOrderBookSubscriber() = default;
};

}  // namespace cex::market_data::domain
