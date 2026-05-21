#pragma once

#include <string>
#include <optional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "app/snapshot_producer.hpp"
#include "domain/depth_curve_builder.hpp"
#include "domain/depth_canonicalizer.hpp"
#include "fob/execution/v1/execution.pb.h"
#include "fob/orders/v1/orders.pb.h"
#include "fob/venue/v1/venue.pb.h"

namespace cex::venues::app {

enum class L3ImpactModel {
  kLinear,
  kQuadratic,
  kSqrt,
};

struct L3ImpactCalibrationConfig {
  bool enabled{false};
  L3ImpactModel model{L3ImpactModel::kLinear};
  std::size_t max_history{256};
  double max_relative_impact{0.5};
  // Weight of execution-based cost in Level-3 blending:
  // S_l3 = (1-w)*S_model + w*S_execution.
  double execution_blend_weight{0.5};
  // Number of samples after which configured weight is fully applied.
  std::size_t min_samples_for_full_weight{32};
};

// F11-AMM-3/4:
// Direct AMM-state -> FOB path (without intermediate synthetic LOB object),
// optional quality comparison against virtual-LOB pipeline,
// and execution-cost calibration knobs (fee/gas/slippage/overhead).
struct DirectAmmFobConfig {
  bool enabled{true};
  bool compare_with_virtual_lob{true};
  std::size_t max_segments_per_side{20};

  // Optional override for AMM pool fee used in direct path.
  // Negative value means "use snapshot taker fee".
  double pool_fee_rate_override{-1.0};

  // Fixed quote-cost correction per execution path.
  double gas_cost_quote{0.0};

  // Additional overhead in bps applied to executable price.
  double execution_overhead_bps{0.0};

  // Price-impact shape multiplier: 1.0 = as-is, >1.0 = steeper impact.
  double slippage_multiplier{1.0};
};

struct CurveQualityThresholds {
  // Legacy F11-CURVE-12 thresholds are normalized [0..1] quality errors.
  // They remain available as an optional pre-filter before confidence scoring.
  double epsilon1_degrade{0.01};
  double epsilon1_disable{0.05};

  double epsilon2_degrade{0.01};
  double epsilon2_disable{0.05};

  double epsilon3_degrade{0.75};
  double epsilon3_disable{0.95};
};

// F11-CURVE-13:
// Confidence and degradation policy for L3 -> L2 -> L1 -> OFF.
//
// Epsilon thresholds are in bps and follow Epsilon.md green-zone defaults.
// Confidence uses normalized epsilon pressure plus stale/error multipliers.
struct CurveDegradationConfig {
  bool enabled{true};

  double epsilon1_green_bps{25.0};
  double epsilon2_green_bps{5.0};
  double epsilon3_green_bps{25.0};

  double min_l3_confidence{0.35};
  double min_l2_confidence{0.10};
  double min_l1_confidence{0.01};

  double stale_confidence_multiplier{0.35};
  double l3_no_execution_confidence_multiplier{0.85};
  double error_confidence_penalty{0.10};

  uint32_t max_consecutive_errors_off{3};

  // F11 status model: stale snapshots must not create new active FOB liquidity.
  // Keep an explicit fallback switch for controlled legacy/incident operation.
  bool publish_stale_l1_fallback{false};
};

// F11 LOB input policy before LOB->FOB conversion.
struct LiquidityCurveInputConfig {
  // Filters venue dust before Q(p), p(q), S(q) construction.
  cex::common::Decimal min_qty{0, 0};

  // Effective executable prices include external taker fees:
  // buy/ask: p*(1+fee), sell/bid: p*(1-fee).
  bool apply_taker_fee{true};
  int32_t fee_adjusted_price_scale{6};
};

// F11-CURVE-14:
// Optional compatibility output derived from VenueLiquidityCurve.
struct SyntheticFlowOrderConfig {
  bool enabled{false};
  std::string topic{"venue.synthetic"};

  // Empty means infer "cex_hedge"/"dex_hedge" from venue_id.
  std::string liquidity_source{};

  // Synthetic orders are short-lived snapshots of venue liquidity.
  int64_t ttl_ms{1000};

  int32_t price_scale{8};
  int32_t quantity_scale{8};
};

// F11-CURVE-15:
// Version metadata for the raw VenueLiquidityCurve payload on venue.liquidity.fob.
// The Kafka value remains a VenueLiquidityCurve protobuf for backward compatibility.
struct LiquidityFobVersioningConfig {
  uint32_t schema_version{1};
  uint32_t min_compatible_schema_version{1};
  std::string producer_version{"f11-curve-15"};
  std::string model_config_version{"default"};
};

// F11-CURVE-9:
// L2 LOB->FOB runtime pipeline:
// canonicalization -> convexification -> regularization -> dual layer -> Kafka publish.
struct LiquidityCurveProducerConfig {
  std::string topic{"venue.liquidity.fob"};
  std::string level{"L3"};
  double tau_ms{1000.0};

  bool apply_convexification{true};
  bool apply_moreau_l2{true};
  bool apply_tikhonov_l2{true};
  bool apply_fenchel_legendre{true};

  domain::ConvexificationConfig convexification{};
  domain::MoreauL2RegularizationConfig moreau_l2{};
  domain::TikhonovL2RegularizationConfig tikhonov_l2{};
  domain::FenchelLegendreConfig fenchel_legendre{};

  // F11-CURVE-1/fees/min-volume input policy for canonical depth.
  LiquidityCurveInputConfig input{};

  // F11-CURVE-10:
  // impact calibrator for Level 3 using execution reports/history.
  L3ImpactCalibrationConfig l3_impact{};

  // F11-AMM-3:
  // direct AMM-state -> FOB path and side-by-side quality comparison.
  DirectAmmFobConfig amm_direct{};

  // F11-CURVE-13:
  // confidence scoring and model-level degradation.
  CurveDegradationConfig degradation{};

  // F11-CURVE-14:
  // optional VenueLiquidityCurve -> SyntheticFlowOrder compatibility output.
  SyntheticFlowOrderConfig synthetic{};

  // F11-CURVE-15:
  // additive schema/producer versioning for venue.liquidity.fob.
  LiquidityFobVersioningConfig liquidity_fob_versioning{};

  // F11-CURVE-12 compatibility:
  // optional normalized quality gating from dev, applied before confidence scoring.
  bool enable_quality_gating{false};
  CurveQualityThresholds quality_thresholds{};
};

// Port: operational storage for generated synthetic_orders.
class ISyntheticOrderRepository {
 public:
  virtual ~ISyntheticOrderRepository() = default;
  virtual bool SaveSyntheticOrder(
      const fob::orders::v1::SyntheticFlowOrder& order) = 0;
};

class LiquidityCurveProducer {
 public:
  struct PublishOptions {
    std::optional<std::string> requested_level;
    std::optional<bool> synthetic_enabled;
  };

  LiquidityCurveProducer(IMessagePublisher* publisher,
                         LiquidityCurveProducerConfig config = {});
  LiquidityCurveProducer(IMessagePublisher* publisher,
                         ISyntheticOrderRepository* synthetic_order_repository,
                         LiquidityCurveProducerConfig config = {});

  // Returns true when a curve was produced and published.
  bool Publish(
      const fob::venue::v1::VenueSnapshot& snapshot,
      fob::venue::v1::VenueLiquidityCurve* published_curve = nullptr,
      std::vector<fob::orders::v1::SyntheticFlowOrder>* published_synthetics = nullptr,
      const PublishOptions& options = {});

  // Feeds execution outcomes for L3 impact calibration.
  void ObserveExecution(const fob::execution::v1::ExecutionIntent& intent,
                        const fob::execution::v1::ExecutionReport& report);

 private:
  struct ImpactSample {
    double qty{0.0};
    double relative_impact{0.0};
    double executed_vwap{0.0};
  };

  struct ImpactModelParams {
    double a{0.0};
    double b{0.0};
    std::size_t sample_count{0};
    double fit_rmse{1.0};
  };

  struct DegradationState {
    uint32_t consecutive_publish_errors{0};
    uint32_t consecutive_execution_errors{0};
  };

  static std::vector<domain::BookLevel> ToBookLevels(
      const google::protobuf::RepeatedPtrField<fob::common::v1::Decimal>& prices,
      const google::protobuf::RepeatedPtrField<fob::common::v1::Decimal>& quantities);

  static fob::venue::v1::SideLiquidityCurve ToProtoCurve(
      const domain::DepthSideCurves& curves);

  void ApplyLiquidityFobVersioning(
      fob::venue::v1::VenueLiquidityCurve* curve) const;
  bool PublishLiquidityFob(
      const fob::venue::v1::VenueLiquidityCurve& curve,
      const std::string& key) const;

  std::vector<fob::orders::v1::SyntheticFlowOrder> BuildSyntheticFlowOrders(
      const fob::venue::v1::VenueLiquidityCurve& curve,
      const SyntheticFlowOrderConfig& synthetic_config) const;
  bool PublishSyntheticFlowOrders(
      const fob::venue::v1::VenueLiquidityCurve& curve,
      const std::string& key,
      const SyntheticFlowOrderConfig& synthetic_config,
      std::vector<fob::orders::v1::SyntheticFlowOrder>* published_orders);

  static domain::DepthSideCurves ApplyL2Pipeline(
      domain::DepthSideCurves curves,
      const LiquidityCurveProducerConfig& config);

  domain::DepthSideCurves ApplyL3ImpactCalibration(
      const domain::DepthSideCurves& curves,
      const std::string& venue_id,
      const std::string& symbol,
      domain::ExecutionSide side) const;

  void UpdateReferenceMid(const fob::venue::v1::VenueSnapshot& snapshot);
  ImpactModelParams CalibrateImpactModel(const std::vector<ImpactSample>& history) const;

  static double EvaluateImpact(const ImpactModelParams& params,
                               L3ImpactModel model,
                               double qty);
  static domain::DepthSideCurves RebuildWithAdjustedMarginalPrice(
      const domain::DepthSideCurves& curves,
      const ImpactModelParams& params,
      const L3ImpactCalibrationConfig& config);
  static double ComputeBlendWeight(const ImpactModelParams& params,
                                   const L3ImpactCalibrationConfig& config);

  static std::string SideKey(const std::string& venue_id,
                             const std::string& symbol,
                             fob::common::v1::Side side);
  static std::string SideKey(const std::string& venue_id,
                             const std::string& symbol,
                             domain::ExecutionSide side);
  static std::string SymbolKey(const std::string& venue_id, const std::string& symbol);

  static double ComputePriceErrorBps(const std::vector<domain::POfQPoint>& base,
                                     const std::vector<domain::POfQPoint>& adjusted);

  static double ComputeShapeErrorBps(const std::vector<domain::POfQPoint>& p_of_q,
                                     const std::vector<domain::SOfQPoint>& s_of_q,
                                     domain::ExecutionSide side);

  static double ComputeRelativeCostError(const std::vector<domain::SOfQPoint>& base,
                                         const std::vector<domain::SOfQPoint>& regularized);
  static double ComputeMonotonicityError(const std::vector<domain::POfQPoint>& p_of_q,
                                         domain::ExecutionSide side);
  static double ComputeConvexityError(const std::vector<domain::SOfQPoint>& s_of_q,
                                      domain::ExecutionSide side);
  static double ComputeDualPenalty(const domain::DepthSideCurves& curves);
  double ComputeExecutionCalibrationError(const std::string& venue_id,
                                         const std::string& symbol,
                                         domain::ExecutionSide side) const;

  double ComputeExecutionCalibrationErrorBps(const domain::DepthSideCurves& curves,
                                             const std::string& venue_id,
                                             const std::string& symbol,
                                             domain::ExecutionSide side) const;
  bool HasExecutionSamples(const std::string& venue_id,
                           const std::string& symbol,
                           domain::ExecutionSide side) const;

  static double Clamp01(double value);

  IMessagePublisher* publisher_;
  ISyntheticOrderRepository* synthetic_order_repository_;
  LiquidityCurveProducerConfig config_;
  mutable std::mutex state_mu_;
  std::unordered_map<std::string, double> reference_mid_by_symbol_;
  std::unordered_map<std::string, std::vector<ImpactSample>> impact_history_by_side_;
  std::unordered_map<std::string, ImpactModelParams> impact_params_by_side_;
  std::unordered_map<std::string, DegradationState> degradation_state_by_symbol_;
};

}  // namespace cex::venues::app
