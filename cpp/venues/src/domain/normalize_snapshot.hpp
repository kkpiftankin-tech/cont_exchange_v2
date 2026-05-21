#pragma once

#include "cex/common/decimal.hpp"
#include "domain/depth_canonicalizer.hpp"
#include "domain/venue_adapter.hpp"
#include "fob/venue/v1/venue.pb.h"

namespace cex::venues::domain {

// Configuration for snapshot normalization (F11-NORM-2 fee/tick/lot scale).
struct NormalizationConfig {
  // Target scale for fee values (e.g. 6 → up to 0.000001 precision).
  int32_t fee_scale{6};

  // Depth canonicalization config (tick/lot alignment, max levels).
  DepthCanonicalizationConfig depth_config;
};

// F11-NORM-1: Convert raw venue snapshot to unified VenueSnapshot proto.
// F11-NORM-2: Quantize fees, tick_size, lot_size to proper Decimal scale.
//
// Pure domain function — no side effects.
fob::venue::v1::VenueSnapshot NormalizeSnapshot(
    const VenueRawSnapshot& raw,
    const NormalizationConfig& config);

// Quantize a Decimal value to target_scale (truncate towards zero).
cex::common::Decimal QuantizeDecimal(const cex::common::Decimal& value,
                                     int32_t target_scale);

}  // namespace cex::venues::domain
