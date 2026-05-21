#pragma once

#include "fob/matching/v1/batch.pb.h"

namespace cex::backtest::app {

// Port for persisting batch outputs into backtest/replay storage (ClickHouse).
class IBatchReplayStorage {
 public:
  virtual ~IBatchReplayStorage() = default;
  virtual bool SaveBatchResult(const fob::matching::v1::BatchResult& evt) = 0;
  virtual bool SaveFills(const fob::matching::v1::BatchResult& evt) = 0;
};

}  // namespace cex::backtest::app
