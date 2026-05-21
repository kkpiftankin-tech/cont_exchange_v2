#pragma once

#include <clickhouse/client.h>

#include "domain/ports/i_order_book_storage.hpp"
#include "infra/clickhouse_storage.hpp"

namespace cex::market_data::infra::clickhouse {

// Not thread-safe
class OrderBookStorage final : public domain::IOrderBookStorage {
 public:
  void Store(const domain::OrderBook&) override;

  explicit OrderBookStorage(const ClickHouseConfig&);

 private:
  ::clickhouse::Client client_;
};

}  // namespace cex::market_data::infra::clickhouse