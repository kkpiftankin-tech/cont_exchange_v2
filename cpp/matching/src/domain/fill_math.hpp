#pragma once

#include <stdexcept>

#include "cex/common/decimal.hpp"
#include "domain/flow_order.hpp"

namespace cex::matching::domain {

struct FilledVolumeUpdate {
  // Cumulative fill in portfolio units.
  cex::common::Decimal new_filled_cum{};
  FlowOrderStatus new_status{FlowOrderStatus::kPartiallyFilled};
};

inline FilledVolumeUpdate ComputeFilledVolumeUpdate(
    const cex::common::Decimal& q_max_portfolio_units,
    const cex::common::Decimal& previous_filled_cum_portfolio_units,
    const cex::common::Decimal& executed_qty_delta_portfolio_units) {
  const cex::common::Decimal zero{0, executed_qty_delta_portfolio_units.scale};
  if (cex::common::Decimal::cmp(executed_qty_delta_portfolio_units, zero) < 0) {
    throw std::invalid_argument("executed_qty_delta_portfolio_units must be non-negative");
  }

  const auto unclamped = cex::common::Decimal::add(
      previous_filled_cum_portfolio_units,
      executed_qty_delta_portfolio_units);
  const auto new_filled_cum = cex::common::Decimal::min(unclamped, q_max_portfolio_units);

  FilledVolumeUpdate update;
  update.new_filled_cum = new_filled_cum;
  update.new_status = cex::common::Decimal::cmp(new_filled_cum, q_max_portfolio_units) >= 0
                          ? FlowOrderStatus::kFilled
                          : FlowOrderStatus::kPartiallyFilled;
  return update;
}

}  // namespace cex::matching::domain
