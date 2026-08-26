#pragma once
// ============================================================================
// vector_flow_segment.hpp — F-05A (T-F05A-201). market_data domain VO.
//
// Векторный flow-сегмент = один столбец матрицы W в пространстве активов, плюс
// параметры сегмента (p_low=0, p_high=dHL, q_rate, q_max). Мапится в proto
// fob.marketdata.v1.VectorFlowSegment при публикации в marketdata.vectorized.
//
// w хранится как double: это структурный коэффициент потока (±1 и ±P_eff),
// вход QP (double, §9 допускает double внутри солвера). Авторитетные денежные
// величины сегмента (q_rate, q_max, effective_price) — Decimal (§9).
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/external_order_level.hpp"

namespace cex::market_data::domain {

struct VectorFlowSegment {
  std::string segment_id;
  std::string source_order_id;
  std::string venue_id;
  std::string pair;
  LevelSide side{LevelSide::kBid};

  std::vector<double> w;  ///< длина = num_assets; столбец W

  cex::common::Decimal p_low{};           ///< 0 (Variant 1)
  cex::common::Decimal p_high{};          ///< = d_hl
  cex::common::Decimal d_hl{};            ///< высота demand-curve (dHL policy)
  cex::common::Decimal q_rate{};          ///< исполняемая скорость (base/сек), capped
  cex::common::Decimal q_max{};           ///< полный base-объём
  cex::common::Decimal effective_price{}; ///< P_eff (solver input)

  std::int64_t source_timestamp_ms{0};
};

}  // namespace cex::market_data::domain
