#pragma once

#include <string>
#include <vector>

#include "cex/common/decimal.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "google/protobuf/timestamp.pb.h"

namespace cex::matching::app {

struct PositionSnapshot {
  std::string provider_id;
  std::string symbol;
  cex::common::Decimal net_qty;
  std::string batch_id;
  fob::common::v1::Decimal clearing_price;
  google::protobuf::Timestamp timestamp;
};

class PositionSnapshotCalculator {
 public:
  std::vector<PositionSnapshot> Calculate(
      const fob::matching::v1::BatchResult& batch) const;
};

}  // namespace cex::matching::app
