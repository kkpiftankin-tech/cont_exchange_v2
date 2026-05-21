#pragma once

#include "domain/entities/order_book.hpp"

namespace cex::market_data::domain {

struct IOrderBookPublisher {
  virtual void Publish(const OrderBook&) = 0;
  
  // New: publish only BBO (lightweight update)
  virtual void PublishBBO(const BBO& bbo, const std::string& symbol, 
                          uint64_t nonce, Timestamp timestamp) = 0;

 protected:
  virtual ~IOrderBookPublisher() {}
};

}  // namespace cex::market_data::domain