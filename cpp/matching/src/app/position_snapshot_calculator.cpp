#include "app/position_snapshot_calculator.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cex::matching::app {

namespace {

struct SnapshotAccumulator {
  std::string provider_id;
  std::string symbol;
  cex::common::Decimal net_qty{cex::common::Decimal::zero()};
};

bool IsValidFill(const fob::matching::v1::FlowFill& fill) {
  if (fill.user_id().empty()) {
    return false;
  }
  if (fill.instrument().symbol().empty()) {
    return false;
  }
  if (fill.side() != fob::common::v1::SIDE_BUY &&
      fill.side() != fob::common::v1::SIDE_SELL) {
    return false;
  }

  const auto qty = cex::common::Decimal::from_proto(fill.executed_qty());
  if (qty.units <= 0) {
    return false;
  }

  return true;
}

std::string SnapshotKey(const std::string& provider_id, const std::string& symbol) {
  static constexpr char kKeySeparator = '\x1f';
  return provider_id + kKeySeparator + symbol;
}

}  // namespace

std::vector<PositionSnapshot> PositionSnapshotCalculator::Calculate(
    const fob::matching::v1::BatchResult& batch) const {
  std::unordered_map<std::string, SnapshotAccumulator> totals;

  for (const auto& fill : batch.fills()) {
    if (!IsValidFill(fill)) {
      continue;
    }

    const auto key = SnapshotKey(fill.user_id(), fill.instrument().symbol());
    auto it = totals.find(key);
    if (it == totals.end()) {
      SnapshotAccumulator seed;
      seed.provider_id = fill.user_id();
      seed.symbol = fill.instrument().symbol();
      it = totals.emplace(key, std::move(seed)).first;
    }

    const auto qty = cex::common::Decimal::from_proto(fill.executed_qty());
    if (fill.side() == fob::common::v1::SIDE_BUY) {
      it->second.net_qty = cex::common::Decimal::add(it->second.net_qty, qty);
      continue;
    }
    it->second.net_qty = cex::common::Decimal::sub(it->second.net_qty, qty);
  }

  std::vector<PositionSnapshot> snapshots;
  snapshots.reserve(totals.size());

  for (const auto& [key, total] : totals) {
    (void)key;
    if (cex::common::Decimal::cmp(total.net_qty, cex::common::Decimal::zero()) == 0) {
      continue;
    }

    PositionSnapshot snapshot;
    snapshot.provider_id = total.provider_id;
    snapshot.symbol = total.symbol;
    snapshot.net_qty = total.net_qty;
    snapshot.batch_id = batch.batch_id();
    snapshot.timestamp = batch.timestamp();

    const auto price_it = batch.clear_prices().find(total.symbol);
    if (price_it != batch.clear_prices().end()) {
      snapshot.clearing_price = price_it->second;
    }

    snapshots.push_back(std::move(snapshot));
  }

  return snapshots;
}

}  // namespace cex::matching::app
