#pragma once

#include <chrono>

#include <boost/uuid/uuid.hpp>

#include "cex/common/decimal.hpp"

namespace cex::market_data::domain {

using Timestamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;

enum class Side {
  Buy,
  Sell,
  Unknown,
};

enum class LiquiditySource {
  Internal,
  CexHedge,
  DexHedge,
  EpsilonMm,
  Unknown,
};

struct FillEvent {
  boost::uuids::uuid fill_id;
  boost::uuids::uuid batch_id;
  boost::uuids::uuid order_id;
  boost::uuids::uuid user_id;
  std::string symbol;
  Side side;
  std::string asset_legs;
  common::Decimal exec_qty;
  common::Decimal exec_price;
  LiquiditySource liquidity_source;
  common::Decimal fees;
  Timestamp timestamp;
};

}  // namespace cex::market_data::domain
