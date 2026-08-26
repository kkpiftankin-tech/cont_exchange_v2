#pragma once
// ============================================================================
// i_vectorized_publisher.hpp — F-05A (T-F05A-205). market_data app port.
//
// Порт публикации векторизованной ликвидности в marketdata.vectorized.
// Реализация — infra::KafkaVectorizedProducer.
// ============================================================================

#include "fob/marketdata/v1/vector_liquidity.pb.h"

namespace cex::market_data::app {

struct IVectorizedPublisher {
  virtual ~IVectorizedPublisher() = default;
  virtual void Publish(
      const fob::marketdata::v1::VectorizedLiquiditySnapshot& snapshot) = 0;
};

}  // namespace cex::market_data::app
