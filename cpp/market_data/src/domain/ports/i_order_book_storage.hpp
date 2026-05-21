#pragma once

#include "domain/entities/order_book.hpp"

namespace cex::market_data::domain {

struct IOrderBookStorage {
  virtual void Store(const OrderBook&) = 0;

 protected:
  virtual ~IOrderBookStorage() = default;
};

}  // namespace cex::market_data::domain
