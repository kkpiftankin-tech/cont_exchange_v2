#include "infra/venue_sim_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "fob/common/v1/common.pb.h"

namespace cex::venues::infra {

namespace {

using fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED;
using fob::execution::v1::ExecutionIntent;
using fob::execution::v1::ExecutionReportStatus;

cex::common::Decimal dec_from_units(const int64_t units, const int32_t scale) {
  cex::common::Decimal out;
  out.units = units;
  out.scale = std::max<int32_t>(0, scale);
  return out;
}

double clamp01(const double v) {
  if (std::isnan(v)) return 1.0;
  return std::clamp(v, 0.0, 1.0);
}

std::string policy_key(const ExecutionIntent& intent) {
  if (!intent.intent_id().empty()) return intent.intent_id();
  return intent.client_order_id();
}

}  // namespace

VenueSimAdapter::VenueSimAdapter(std::string venue_id,
                                 const domain::VenueType venue_type,
                                 std::string session_id)
    : venue_id_(std::move(venue_id)),
      venue_type_(venue_type),
      session_id_(std::move(session_id)),
      inner_(std::make_unique<SimulatedVenueAdapter>(venue_id_, venue_type_)) {}

std::string VenueSimAdapter::VenueId() const {
  return venue_id_;
}

domain::VenueType VenueSimAdapter::Type() const {
  return venue_type_;
}

const std::string& VenueSimAdapter::SessionId() const {
  return session_id_;
}

bool VenueSimAdapter::Connect() {
  return inner_->Connect();
}

bool VenueSimAdapter::Subscribe(
    const std::vector<domain::VenueSubscription>& subscriptions) {
  return inner_->Subscribe(subscriptions);
}

bool VenueSimAdapter::Reconnect() {
  return inner_->Reconnect();
}

domain::VenueHeartbeat VenueSimAdapter::Heartbeat() {
  return inner_->Heartbeat();
}

std::optional<domain::VenueRawSnapshot> VenueSimAdapter::RequestSnapshot(
    const domain::VenueSnapshotRequest& request) {
  auto snapshot = inner_->RequestSnapshot(request);
  if (snapshot.has_value()) {
    std::lock_guard<std::mutex> lock(mu_);
    last_snapshot_ = *snapshot;
  }
  return snapshot;
}

void VenueSimAdapter::SetLastSnapshot(const domain::VenueRawSnapshot& snapshot) {
  std::lock_guard<std::mutex> lock(mu_);
  last_snapshot_ = snapshot;
}

std::optional<domain::VenueRawSnapshot> VenueSimAdapter::LastSnapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_snapshot_;
}

bool VenueSimAdapter::ApplyRuntimeConfig(
    const domain::VenueAdapterRuntimeConfig& config) {
  return inner_->ApplyRuntimeConfig(config);
}

void VenueSimAdapter::SetDefaultOrderPolicy(const OrderPolicy& policy) {
  std::lock_guard<std::mutex> lock(mu_);
  default_policy_ = policy;
  default_policy_.fill_ratio = clamp01(default_policy_.fill_ratio);
}

void VenueSimAdapter::SetOrderPolicyFor(const std::string& key,
                                        const OrderPolicy& policy) {
  if (key.empty()) return;
  OrderPolicy normalized = policy;
  normalized.fill_ratio = clamp01(normalized.fill_ratio);
  std::lock_guard<std::mutex> lock(mu_);
  per_intent_policies_[key] = normalized;
}

uint64_t VenueSimAdapter::SentOrderCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sent_orders_;
}

VenueSimAdapter::OrderPolicy VenueSimAdapter::ResolvePolicy(
    const ExecutionIntent& intent) const {
  const std::string key = policy_key(intent);
  std::lock_guard<std::mutex> lock(mu_);
  if (!key.empty()) {
    const auto it = per_intent_policies_.find(key);
    if (it != per_intent_policies_.end()) return it->second;
  }
  return default_policy_;
}

namespace {

// F12-BACKTEST-2 — walk a LOB side accumulating up to target_qty_units of
// base. Returns (filled_units, remaining_units, vwap_price_units). When the
// requested side has no liquidity returns filled=0, vwap=0.
struct BookWalkResult {
  int64_t filled_units{0};
  int64_t remaining_units{0};
  int64_t vwap_price_units{0};
  int32_t price_scale{0};
  int32_t qty_scale{0};
  __int128 filled_notional_x{0};  // sum(level_qty_units * level_price_units)
  bool side_empty{false};
};

BookWalkResult walk_book(const std::vector<domain::VenueBookLevel>& levels,
                         const int64_t target_qty_units,
                         const int32_t target_qty_scale) {
  BookWalkResult out;
  out.qty_scale = target_qty_scale;
  if (levels.empty()) {
    out.side_empty = true;
    out.remaining_units = target_qty_units;
    return out;
  }
  out.price_scale = levels.front().price.scale;

  int64_t remaining = target_qty_units;
  __int128 notional_x = 0;
  int64_t filled = 0;

  for (const auto& level : levels) {
    if (remaining <= 0) break;
    // Levels coming from SimulatedVenueAdapter share a consistent qty_scale.
    // Defensive guard: skip mis-scaled rows rather than crash.
    if (level.qty.units <= 0 || level.qty.scale != target_qty_scale) continue;

    const int64_t take = std::min<int64_t>(level.qty.units, remaining);
    notional_x += static_cast<__int128>(take) *
                  static_cast<__int128>(level.price.units);
    filled += take;
    remaining -= take;
  }

  out.filled_units = filled;
  out.remaining_units = std::max<int64_t>(0, target_qty_units - filled);
  out.filled_notional_x = notional_x;
  if (filled > 0) {
    out.vwap_price_units = static_cast<int64_t>(notional_x / static_cast<__int128>(filled));
  }
  return out;
}

int32_t compute_slippage_bps(const int64_t avg_price_units,
                             const int64_t reference_units,
                             const fob::common::v1::Side side) {
  if (reference_units == 0 || avg_price_units == 0) return 0;
  const double avg = static_cast<double>(avg_price_units);
  const double ref = static_cast<double>(reference_units);
  double signed_slip = (avg - ref) / ref;
  if (side == fob::common::v1::SIDE_SELL) signed_slip = -signed_slip;
  return static_cast<int32_t>(std::llround(signed_slip * 10000.0));
}

}  // namespace

domain::VenueOrderResult VenueSimAdapter::BuildResult(
    const ExecutionIntent& intent,
    const OrderPolicy& policy) const {
  domain::VenueOrderResult result;
  result.venue_order_id = "VSIM-" +
      (intent.intent_id().empty() ? intent.client_order_id() : intent.intent_id());
  result.latency_ms = policy.latency_ms;

  // Explicit rejection wins over every other policy field.
  if (!policy.reject_code.empty() || !policy.reject_message.empty()) {
    result.accepted = false;
    result.status = EXECUTION_REPORT_STATUS_REJECTED;
    result.error_code = policy.reject_code.empty() ? "VENUE_SIM_REJECTED"
                                                    : policy.reject_code;
    result.error_message = policy.reject_message;
    return result;
  }

  const cex::common::Decimal target_qty =
      cex::common::Decimal::from_proto(intent.target_qty());
  const int32_t qty_scale = target_qty.scale;

  // F12-BACKTEST-2 — derive RawExecutionEvent from historical LOB.
  if (policy.walk_book) {
    std::optional<domain::VenueRawSnapshot> snapshot_copy;
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot_copy = last_snapshot_;
    }
    if (!snapshot_copy.has_value()) {
      result.accepted = false;
      result.status = EXECUTION_REPORT_STATUS_REJECTED;
      result.error_code = "VENUE_SIM_NO_SNAPSHOT";
      result.error_message =
          "walk_book requested but no historical snapshot has been injected";
      return result;
    }

    const auto& snapshot = *snapshot_copy;
    const bool buy_side = intent.side() != fob::common::v1::SIDE_SELL;
    const auto& levels = buy_side ? snapshot.asks : snapshot.bids;

    const BookWalkResult walk = walk_book(levels, target_qty.units, qty_scale);
    if (walk.side_empty || walk.filled_units == 0) {
      result.accepted = false;
      result.status = EXECUTION_REPORT_STATUS_REJECTED;
      result.error_code = "INSUFFICIENT_LIQUIDITY";
      result.error_message = "VenueSim book side has no fillable liquidity";
      result.remaining_qty = dec_from_units(target_qty.units, qty_scale);
      return result;
    }

    // Limit-price enforcement against the VWAP.
    if (policy.enforce_limit_price &&
        intent.has_limit_price() && intent.limit_price().units() != 0) {
      const cex::common::Decimal limit =
          cex::common::Decimal::from_proto(intent.limit_price());
      const bool breached =
          buy_side ? (walk.vwap_price_units > limit.units)
                   : (walk.vwap_price_units < limit.units);
      if (breached) {
        result.accepted = false;
        result.status = EXECUTION_REPORT_STATUS_CANCELLED;
        result.error_code = "PRICE_LIMIT_EXCEEDED";
        result.error_message = "VenueSim VWAP breached intent limit_price";
        result.remaining_qty = dec_from_units(target_qty.units, qty_scale);
        return result;
      }
    }

    result.filled_qty = dec_from_units(walk.filled_units, qty_scale);
    result.remaining_qty = dec_from_units(walk.remaining_units, qty_scale);
    result.average_price = dec_from_units(
        walk.vwap_price_units + policy.slippage_units, walk.price_scale);

    // Slippage vs reference (policy override → limit_price → snapshot.mid).
    int64_t reference_units = policy.reference_mid_units;
    if (reference_units == 0 && intent.has_limit_price() &&
        intent.limit_price().units() != 0) {
      reference_units = intent.limit_price().units();
    }
    if (reference_units == 0) {
      reference_units = snapshot.mid_price.units;
    }
    result.slippage_bps = compute_slippage_bps(
        result.average_price.units, reference_units, intent.side());

    // Fees: taker rate * filled_notional. Notional uses the post-slippage
    // VWAP so fees consistently reflect the price actually paid.
    if (policy.taker_fee_bps != 0) {
      const __int128 effective_price_x =
          static_cast<__int128>(result.average_price.units);
      const __int128 notional_x =
          static_cast<__int128>(walk.filled_units) * effective_price_x;
      // fee_x has scale (price_scale + qty_scale + 4) before /10000. Final
      // scale we publish equals price_scale + qty_scale (Money.amount):
      // canonical for an absolute fee in quote currency.
      const __int128 fee_x = (notional_x * static_cast<__int128>(policy.taker_fee_bps)) / 10000;
      result.fee = dec_from_units(static_cast<int64_t>(fee_x),
                                   result.average_price.scale + qty_scale);
      result.fee_rate = dec_from_units(policy.taker_fee_bps, 4);  // 10 bps → 0.0010
    }
    result.fee_currency = policy.fee_currency.empty()
                              ? intent.instrument().quote()
                              : policy.fee_currency;

    ExecutionReportStatus status = policy.forced_status;
    if (status == EXECUTION_REPORT_STATUS_UNSPECIFIED) {
      status = walk.remaining_units == 0
                   ? EXECUTION_REPORT_STATUS_FILLED
                   : EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
    }
    result.status = status;
    result.accepted = status != EXECUTION_REPORT_STATUS_REJECTED &&
                      status != EXECUTION_REPORT_STATUS_CANCELLED;
    return result;
  }

  // F12-BACKTEST-1 path — coarse fill_ratio policy.
  const double fill_ratio = clamp01(policy.fill_ratio);
  const int64_t filled_units = static_cast<int64_t>(std::llround(
      static_cast<double>(target_qty.units) * fill_ratio));
  const int64_t remaining_units = std::max<int64_t>(0, target_qty.units - filled_units);
  result.filled_qty = dec_from_units(filled_units, qty_scale);
  result.remaining_qty = dec_from_units(remaining_units, qty_scale);

  if (intent.has_limit_price() && intent.limit_price().units() != 0) {
    const cex::common::Decimal limit =
        cex::common::Decimal::from_proto(intent.limit_price());
    result.average_price = dec_from_units(limit.units + policy.slippage_units, limit.scale);
  } else {
    const domain::VenueOrderResult inner = inner_->SendOrder(intent);
    if (inner.accepted) {
      result.average_price = dec_from_units(
          inner.average_price.units + policy.slippage_units,
          inner.average_price.scale);
    } else {
      result.accepted = false;
      result.status = EXECUTION_REPORT_STATUS_REJECTED;
      result.error_code = inner.error_code.empty() ? "VENUE_SIM_DISCONNECTED"
                                                    : inner.error_code;
      result.error_message = inner.error_message;
      return result;
    }
  }

  // Slippage + fees on the fill_ratio path too — backtest harnesses still
  // want these signals even without a book walk.
  int64_t reference_units = policy.reference_mid_units;
  if (reference_units == 0 && intent.has_limit_price() &&
      intent.limit_price().units() != 0) {
    reference_units = intent.limit_price().units();
  }
  result.slippage_bps = compute_slippage_bps(
      result.average_price.units, reference_units, intent.side());
  if (filled_units > 0 && policy.taker_fee_bps != 0) {
    const __int128 notional_x =
        static_cast<__int128>(filled_units) *
        static_cast<__int128>(result.average_price.units);
    const __int128 fee_x =
        (notional_x * static_cast<__int128>(policy.taker_fee_bps)) / 10000;
    result.fee = dec_from_units(static_cast<int64_t>(fee_x),
                                 result.average_price.scale + qty_scale);
    result.fee_rate = dec_from_units(policy.taker_fee_bps, 4);
  }
  result.fee_currency = policy.fee_currency.empty()
                            ? intent.instrument().quote()
                            : policy.fee_currency;

  ExecutionReportStatus status = policy.forced_status;
  if (status == EXECUTION_REPORT_STATUS_UNSPECIFIED) {
    if (filled_units == 0) {
      status = EXECUTION_REPORT_STATUS_CANCELLED;
    } else if (remaining_units == 0) {
      status = EXECUTION_REPORT_STATUS_FILLED;
    } else {
      status = EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
    }
  }

  result.accepted = status != EXECUTION_REPORT_STATUS_REJECTED &&
                    status != EXECUTION_REPORT_STATUS_CANCELLED;
  result.status = status;
  return result;
}

domain::VenueOrderResult VenueSimAdapter::SendOrder(
    const ExecutionIntent& intent) {
  // Heartbeat check — VenueSim only "rejects" when the inner adapter is in
  // a disconnected state, so backtest harnesses can still simulate venue
  // outages via Reconnect/Disconnect cycles.
  const auto hb = inner_->Heartbeat();
  if (hb.status == domain::VenueConnectionStatus::kDisconnected) {
    domain::VenueOrderResult result;
    result.accepted = false;
    result.status = EXECUTION_REPORT_STATUS_REJECTED;
    result.venue_order_id = "VSIM-" +
        (intent.intent_id().empty() ? intent.client_order_id() : intent.intent_id());
    result.error_code = "VENUE_SIM_DISCONNECTED";
    result.error_message = "VenueSim adapter is disconnected";
    return result;
  }

  const OrderPolicy policy = ResolvePolicy(intent);
  domain::VenueOrderResult out = BuildResult(intent, policy);

  std::lock_guard<std::mutex> lock(mu_);
  ++sent_orders_;
  return out;
}

}  // namespace cex::venues::infra
