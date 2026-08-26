#pragma once
// ============================================================================
// vectorize.hpp — F-05A (T-F05A-202). market_data domain.
//
// Векторизация: набор дискретных внешних уровней + стабильный AssetBasis →
// векторные flow-сегменты (столбцы W). Чистая, детерминированная логика (для
// replay). Публикация в marketdata.vectorized и CH-persist — в UC (T-F05A-205/206).
//
// w_i (R-F05A-001):  bid: w = e_X − P_eff·e_Y ;  ask: w = −e_X + P_eff·e_Y.
// Сегмент (R-F05A-003): p_low=0, p_high=d_hl, d_hl = dHL-policy(P_eff),
//   q_max = remaining|quantity (base), q_rate = min(q_max, rate_cap).
// ============================================================================

#include <string>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/asset_basis.hpp"
#include "domain/external_order_level.hpp"
#include "domain/vector_flow_segment.hpp"

namespace cex::market_data::domain {

struct VectorizeConfig {
  /// dHL-policy (default): d_hl = dhl_fraction · P_eff. Плагируемо (ADR: dHL —
  /// политика, а не фикс. форма); при интеграции берётся из solver_config.
  double dhl_fraction{0.01};
  /// Кэп скорости: q_rate = min(q_max, rate_cap). units==0 ⇒ без кэпа.
  cex::common::Decimal rate_cap{};
  std::int32_t decimal_scale{12};
};

struct SkippedLevel {
  std::string source_order_id;
  std::string reason;
};

struct VectorizeResult {
  AssetBasis basis;
  std::vector<VectorFlowSegment> segments;
  std::vector<SkippedLevel> skipped;
};

/// Стабильный базис активов (лексикографический union base+quote всех уровней).
AssetBasis BuildAssetBasis(const std::vector<ExternalOrderLevel>& levels);

/// Векторизовать уровни в сегменты над общим базисом. Невалидные (q<=0,
/// неизвестный актив, base==quote) — в skipped, без прерывания.
VectorizeResult Vectorize(const std::vector<ExternalOrderLevel>& levels,
                          const VectorizeConfig& cfg = {});

}  // namespace cex::market_data::domain
