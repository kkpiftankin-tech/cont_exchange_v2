#pragma once

#include <optional>
#include <string>

#include "fob/venue/v1/venue.pb.h"

namespace cex::market_data::domain {

// Interface for storing and retrieving liquidity curves
class ILiquidityCurveStorage {
 public:
  virtual ~ILiquidityCurveStorage() = default;

  // Store a liquidity curve (both bid and ask sides)
  virtual void Store(const fob::venue::v1::VenueLiquidityCurve& curve) = 0;

  // Get a specific side of the liquidity curve for a venue and symbol
  virtual std::optional<fob::venue::v1::SideLiquidityCurve> GetCurve(
      const std::string& venue,
      const std::string& symbol,
      fob::venue::v1::ExecutionSide side) const = 0;
};

}  // namespace cex::market_data::domain
