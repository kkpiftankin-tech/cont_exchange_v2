#pragma once
// ============================================================================
// i_ce_clearing_publisher.hpp — F-05A CE (ADR-055/056, вариант A). market_data app port.
//
// Порт публикации CeClearingInput (book-derived агенты + марки) в ce.clearing.input.
// Реализация — infra::KafkaCeClearingProducer. Потребитель — matching (on_ce_clearing_input).
// ============================================================================

#include "fob/marketdata/v1/vector_liquidity.pb.h"

namespace cex::market_data::app {

struct ICeClearingPublisher {
  virtual ~ICeClearingPublisher() = default;
  virtual void Publish(const fob::marketdata::v1::CeClearingInput& input) = 0;
};

}  // namespace cex::market_data::app
