#include "infra/cex_local_lob_assembler.hpp"

#include <algorithm>

namespace cex::venues::infra {

CexLocalLobAssembler::CexLocalLobAssembler(const std::size_t max_levels)
    : max_levels_(std::max<std::size_t>(1, max_levels)) {}

void CexLocalLobAssembler::ConfigureMaxLevels(const std::size_t max_levels) {
  max_levels_ = std::max<std::size_t>(1, max_levels);
  if (initialized_) {
    (void)RebuildTopLevels();
  }
}

void CexLocalLobAssembler::Reset() {
  initialized_ = false;
  last_update_id_ = 0;
  price_scale_ = 0;
  qty_scale_ = 0;
  bid_map_.clear();
  ask_map_.clear();
  bids_.clear();
  asks_.clear();
}

bool CexLocalLobAssembler::IsInitialized() const {
  return initialized_;
}

uint64_t CexLocalLobAssembler::LastUpdateId() const {
  return last_update_id_;
}

bool CexLocalLobAssembler::InitializeFromSnapshot(const Snapshot& snapshot) {
  if (snapshot.bids.empty() || snapshot.asks.empty()) return false;

  Reset();

  price_scale_ = snapshot.bids.front().price.scale;
  qty_scale_ = snapshot.bids.front().qty.scale;

  for (const auto& level : snapshot.bids) {
    if (level.price.units <= 0 || level.qty.units <= 0) continue;
    bid_map_[level.price.units] = level.qty.units;
  }
  for (const auto& level : snapshot.asks) {
    if (level.price.units <= 0 || level.qty.units <= 0) continue;
    ask_map_[level.price.units] = level.qty.units;
  }

  if (!RebuildTopLevels()) {
    Reset();
    return false;
  }

  // Crossed book after snapshot means the local state cannot be trusted.
  if (bids_.front().price.units >= asks_.front().price.units) {
    Reset();
    return false;
  }

  initialized_ = true;
  last_update_id_ = snapshot.last_update_id;
  return true;
}

CexLocalLobAssembler::ApplyStatus CexLocalLobAssembler::ApplyDiff(const DiffEvent& diff) {
  if (!initialized_) return ApplyStatus::kNeedReinit;
  if (diff.first_update_id == 0 || diff.final_update_id == 0) return ApplyStatus::kInvalid;
  if (diff.final_update_id < diff.first_update_id) return ApplyStatus::kInvalid;

  const uint64_t expected = last_update_id_ + 1;
  if (diff.final_update_id < expected) return ApplyStatus::kIgnoredOld;
  if (diff.first_update_id > expected) {
    Reset();
    return ApplyStatus::kNeedReinit;
  }

  for (const auto& level : diff.bid_deltas) {
    if (level.price.units <= 0) continue;
    if (level.qty.units <= 0) {
      bid_map_.erase(level.price.units);
    } else {
      bid_map_[level.price.units] = level.qty.units;
    }
  }
  for (const auto& level : diff.ask_deltas) {
    if (level.price.units <= 0) continue;
    if (level.qty.units <= 0) {
      ask_map_.erase(level.price.units);
    } else {
      ask_map_[level.price.units] = level.qty.units;
    }
  }

  if (!RebuildTopLevels()) {
    Reset();
    return ApplyStatus::kNeedReinit;
  }

  if (bids_.front().price.units >= asks_.front().price.units) {
    Reset();
    return ApplyStatus::kNeedReinit;
  }

  last_update_id_ = diff.final_update_id;
  return ApplyStatus::kApplied;
}

const std::vector<domain::VenueBookLevel>& CexLocalLobAssembler::bids() const {
  return bids_;
}

const std::vector<domain::VenueBookLevel>& CexLocalLobAssembler::asks() const {
  return asks_;
}

bool CexLocalLobAssembler::RebuildTopLevels() {
  bids_.clear();
  asks_.clear();

  bids_.reserve(std::min<std::size_t>(max_levels_, bid_map_.size()));
  asks_.reserve(std::min<std::size_t>(max_levels_, ask_map_.size()));

  std::size_t count = 0;
  for (const auto& [price, qty] : bid_map_) {
    if (count++ >= max_levels_) break;
    if (qty <= 0) continue;
    bids_.push_back(domain::VenueBookLevel{
        .price = cex::common::Decimal{price, price_scale_},
        .qty = cex::common::Decimal{qty, qty_scale_},
    });
  }

  count = 0;
  for (const auto& [price, qty] : ask_map_) {
    if (count++ >= max_levels_) break;
    if (qty <= 0) continue;
    asks_.push_back(domain::VenueBookLevel{
        .price = cex::common::Decimal{price, price_scale_},
        .qty = cex::common::Decimal{qty, qty_scale_},
    });
  }

  return !bids_.empty() && !asks_.empty();
}

}  // namespace cex::venues::infra
