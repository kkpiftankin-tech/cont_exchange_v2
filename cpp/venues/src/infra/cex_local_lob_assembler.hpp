#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "domain/venue_adapter.hpp"

namespace cex::venues::infra {

class CexLocalLobAssembler {
 public:
  struct Snapshot {
    uint64_t last_update_id{0};
    std::vector<domain::VenueBookLevel> bids;
    std::vector<domain::VenueBookLevel> asks;
  };

  struct DiffEvent {
    uint64_t first_update_id{0};
    uint64_t final_update_id{0};
    std::vector<domain::VenueBookLevel> bid_deltas;
    std::vector<domain::VenueBookLevel> ask_deltas;
  };

  enum class ApplyStatus {
    kApplied,
    kIgnoredOld,
    kNeedReinit,
    kInvalid,
  };

  explicit CexLocalLobAssembler(std::size_t max_levels = 50);

  void ConfigureMaxLevels(std::size_t max_levels);
  void Reset();

  bool IsInitialized() const;
  uint64_t LastUpdateId() const;

  bool InitializeFromSnapshot(const Snapshot& snapshot);
  ApplyStatus ApplyDiff(const DiffEvent& diff);

  const std::vector<domain::VenueBookLevel>& bids() const;
  const std::vector<domain::VenueBookLevel>& asks() const;

 private:
  bool RebuildTopLevels();

  std::size_t max_levels_{50};
  bool initialized_{false};
  uint64_t last_update_id_{0};

  int32_t price_scale_{0};
  int32_t qty_scale_{0};

  std::map<int64_t, int64_t, std::greater<int64_t>> bid_map_;
  std::map<int64_t, int64_t> ask_map_;
  std::vector<domain::VenueBookLevel> bids_;
  std::vector<domain::VenueBookLevel> asks_;
};

}  // namespace cex::venues::infra
