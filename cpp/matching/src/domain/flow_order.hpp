#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cex/common/decimal.hpp"
#include "fob/orders/v1/orders.pb.h"

namespace cex::matching::domain {

enum class FlowOrderTimeInForce {
  kGtc,
  kGtd,
  kIoc,
};

enum class FlowOrderStatus {
  kNew,
  kActive,
  kPartiallyFilled,
  kFilled,
  kCancelled,
  kExpired,
  kLiquidated,
};

struct FlowOrderLeg {
  std::string instrument_symbol;
  cex::common::Decimal weight{};

  [[nodiscard]] bool has_non_zero_weight() const {
    const cex::common::Decimal zero{0, weight.scale};
    return cex::common::Decimal::cmp(weight, zero) != 0;
  }
};

struct FlowOrder {
  std::string order_id;
  std::string user_id;
  std::vector<FlowOrderLeg> legs;

  cex::common::Decimal p_low{};
  cex::common::Decimal p_high{};
  cex::common::Decimal q_rate{};
  cex::common::Decimal q_max{};
  cex::common::Decimal filled_cum{};

  FlowOrderTimeInForce time_in_force{FlowOrderTimeInForce::kGtc};
  std::chrono::system_clock::time_point window_start{};
  std::optional<std::chrono::system_clock::time_point> window_end;

  FlowOrderStatus status{FlowOrderStatus::kNew};
  std::chrono::system_clock::time_point created_at{};
  std::chrono::system_clock::time_point updated_at{};

  static FlowOrder from_proto(fob::orders::v1::FlowOrder proto_order) {
    FlowOrder order;

    const auto ts_to_tp = [](const google::protobuf::Timestamp& ts) {
      const auto d = std::chrono::seconds{ts.seconds()} +
                     std::chrono::nanoseconds{ts.nanos()};
      return std::chrono::system_clock::time_point{
          std::chrono::duration_cast<std::chrono::system_clock::duration>(d)};
    };

    const auto map_status = [](const fob::common::v1::OrderStatus status) {
      switch (status) {
        case fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED:
          return FlowOrderStatus::kPartiallyFilled;
        case fob::common::v1::ORDER_STATUS_FILLED:
          return FlowOrderStatus::kFilled;
        case fob::common::v1::ORDER_STATUS_CANCELED:
          return FlowOrderStatus::kCancelled;
        case fob::common::v1::ORDER_STATUS_EXPIRED:
          return FlowOrderStatus::kExpired;
        case fob::common::v1::ORDER_STATUS_REJECTED:
          return FlowOrderStatus::kLiquidated;
        case fob::common::v1::ORDER_STATUS_NEW:
          return FlowOrderStatus::kNew;
        case fob::common::v1::ORDER_STATUS_UNSPECIFIED:
        default:
          return FlowOrderStatus::kActive;
      }
    };

    const auto map_tif = [](const fob::common::v1::TimeInForce tif) {
      switch (tif) {
        case fob::common::v1::TIF_IOC:
        case fob::common::v1::TIF_FOK:
          return FlowOrderTimeInForce::kIoc;
        case fob::common::v1::TIF_GTC:
        case fob::common::v1::TIF_UNSPECIFIED:
        default:
          return FlowOrderTimeInForce::kGtc;
      }
    };

    order.order_id = proto_order.order_id();
    order.user_id = proto_order.user_id();

    std::string symbol = proto_order.instrument().symbol();
    if (symbol.empty() &&
        !proto_order.instrument().base().empty() &&
        !proto_order.instrument().quote().empty()) {
      symbol = proto_order.instrument().base() + "/" + proto_order.instrument().quote();
    }

    const bool is_sell = (proto_order.side() == fob::common::v1::SIDE_SELL);

    FlowOrderLeg leg;
    leg.instrument_symbol = symbol;
    leg.weight = is_sell ? cex::common::Decimal{-1, 0} : cex::common::Decimal{1, 0};
    order.legs.push_back(std::move(leg));
    order.sort_legs_by_symbol();

    const auto price_low = proto_order.has_price_low()
        ? cex::common::Decimal::from_proto(proto_order.price_low())
        : cex::common::Decimal::zero();
    const auto price_high = proto_order.has_price_high()
        ? cex::common::Decimal::from_proto(proto_order.price_high())
        : cex::common::Decimal::zero();

    if (is_sell) {
      order.p_low = cex::common::Decimal::sub({0, 0}, price_high);
      order.p_high = cex::common::Decimal::sub({0, 0}, price_low);
    } else {
      order.p_low = price_low;
      order.p_high = price_high;
    }

    order.q_rate = proto_order.has_max_speed()
        ? cex::common::Decimal::from_proto(proto_order.max_speed())
        : cex::common::Decimal{};
    order.q_max = proto_order.has_total_qty()
        ? cex::common::Decimal::from_proto(proto_order.total_qty())
        : cex::common::Decimal{};

    const auto remaining = proto_order.has_remaining_qty()
        ? cex::common::Decimal::from_proto(proto_order.remaining_qty())
        : order.q_max;

    order.filled_cum = cex::common::Decimal::sub(order.q_max, remaining);

    const cex::common::Decimal zero{0, order.filled_cum.scale};
    if (cex::common::Decimal::cmp(order.filled_cum, zero) < 0) {
      order.filled_cum = zero;
    }
    if (cex::common::Decimal::cmp(order.filled_cum, order.q_max) > 0) {
      order.filled_cum = order.q_max;
    }

    order.time_in_force = map_tif(proto_order.tif());
    order.status = map_status(proto_order.status());

    const auto now = std::chrono::system_clock::now();
    order.created_at = proto_order.has_created_at() ? ts_to_tp(proto_order.created_at()) : now;
    order.updated_at = proto_order.has_updated_at() ? ts_to_tp(proto_order.updated_at())
                                                    : order.created_at;
    order.window_start = order.created_at;
    order.window_end = std::nullopt;

    return order;
  }

  [[nodiscard]] const FlowOrderLeg* find_leg(std::string_view instrument_symbol) const {
    const auto it = std::find_if(legs.begin(), legs.end(), [&](const FlowOrderLeg& leg) {
      return leg.instrument_symbol == instrument_symbol;
    });
    if (it == legs.end()) {
      return nullptr;
    }
    return &(*it);
  }

  [[nodiscard]] bool has_leg(std::string_view instrument_symbol) const {
    return find_leg(instrument_symbol) != nullptr;
  }

  void sort_legs_by_symbol() {
    std::sort(legs.begin(), legs.end(), [](const FlowOrderLeg& lhs, const FlowOrderLeg& rhs) {
      return lhs.instrument_symbol < rhs.instrument_symbol;
    });
  }

  [[nodiscard]] bool has_valid_legs() const {
    if (legs.empty()) {
      return false;
    }

    std::string previous_symbol;
    bool has_previous = false;
    for (const auto& leg : legs) {
      if (leg.instrument_symbol.empty()) {
        return false;
      }
      if (!leg.has_non_zero_weight()) {
        return false;
      }
      if (has_previous && !(previous_symbol < leg.instrument_symbol)) {
        return false;
      }
      previous_symbol = leg.instrument_symbol;
      has_previous = true;
    }
    return true;
  }

  [[nodiscard]] bool has_valid_fill_state() const {
    return cex::common::Decimal::cmp(filled_cum, q_max) <= 0;
  }

  [[nodiscard]] cex::common::Decimal remaining_qty() const {
    const auto remaining = cex::common::Decimal::sub(q_max, filled_cum);
    const cex::common::Decimal zero{0, remaining.scale};
    if (cex::common::Decimal::cmp(remaining, zero) < 0) {
      return zero;
    }
    return remaining;
  }

  [[nodiscard]] bool is_active_for_batch() const {
    return status == FlowOrderStatus::kActive || status == FlowOrderStatus::kPartiallyFilled;
  }

  [[nodiscard]] bool is_matchable_at(std::chrono::system_clock::time_point as_of_time) const {
    if (time_in_force == FlowOrderTimeInForce::kIoc) {
      return false;
    }
    if (!is_active_for_batch()) {
      return false;
    }
    if (cex::common::Decimal::cmp(filled_cum, q_max) >= 0) {
      return false;
    }
    if (window_start > as_of_time) {
      return false;
    }
    if (window_end.has_value() && as_of_time >= *window_end) {
      return false;
    }
    return true;
  }
};

}  // namespace cex::matching::domain
