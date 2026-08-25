#pragma once

#include <clickhouse/client.h>
#include <string>

#include "infra/clickhouse_storage.hpp"
#include "fob/venue/v1/venue.pb.h"

namespace cex::market_data::infra::clickhouse {

// Storage for VenueLiquidityCurve in ClickHouse for historical analytics
class ClickHouseLiquidityCurveStorage {
 public:
  explicit ClickHouseLiquidityCurveStorage(const ClickHouseConfig& config);
  
  // Ensure table schema exists
  void EnsureSchema();
  
  // Save a liquidity curve to ClickHouse
  void Save(const fob::venue::v1::VenueLiquidityCurve& curve);

 private:
  ::clickhouse::Client client_;
  std::string table_name_;
  std::string database_;
  int retention_days_{90};
};

}  // namespace cex::market_data::infra::clickhouse
