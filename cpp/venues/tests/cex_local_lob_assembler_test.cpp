#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "infra/cex_local_lob_assembler.hpp"

namespace {

using cex::venues::domain::VenueBookLevel;
using cex::venues::infra::CexLocalLobAssembler;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

VenueBookLevel Level(const int64_t price_units,
                     const int32_t price_scale,
                     const int64_t qty_units,
                     const int32_t qty_scale) {
  return VenueBookLevel{
      .price = cex::common::Decimal{price_units, price_scale},
      .qty = cex::common::Decimal{qty_units, qty_scale},
  };
}

CexLocalLobAssembler::Snapshot BaseSnapshot() {
  CexLocalLobAssembler::Snapshot snapshot;
  snapshot.last_update_id = 100;
  snapshot.bids = {
      Level(10000, 2, 1200, 3),
      Level(9990, 2, 1500, 3),
  };
  snapshot.asks = {
      Level(10010, 2, 1100, 3),
      Level(10020, 2, 2000, 3),
  };
  return snapshot;
}

bool IsSortedDescByPrice(const std::vector<VenueBookLevel>& side) {
  if (side.empty()) return true;
  for (std::size_t i = 1; i < side.size(); ++i) {
    if (side[i - 1].price.units < side[i].price.units) return false;
  }
  return true;
}

bool IsSortedAscByPrice(const std::vector<VenueBookLevel>& side) {
  if (side.empty()) return true;
  for (std::size_t i = 1; i < side.size(); ++i) {
    if (side[i - 1].price.units > side[i].price.units) return false;
  }
  return true;
}

bool TestInitializeAndApplySequentialDiffs() {
  CexLocalLobAssembler lob(10);
  if (!Check(lob.InitializeFromSnapshot(BaseSnapshot()), "Snapshot init must succeed")) {
    return false;
  }

  CexLocalLobAssembler::DiffEvent diff1;
  diff1.first_update_id = 101;
  diff1.final_update_id = 102;
  diff1.bid_deltas = {
      Level(10000, 2, 1800, 3),  // update best bid qty
      Level(9980, 2, 900, 3),    // add deeper bid
  };
  diff1.ask_deltas = {
      Level(10010, 2, 0, 3),     // delete best ask
      Level(10015, 2, 700, 3),   // add new best ask
  };

  if (!Check(lob.ApplyDiff(diff1) == CexLocalLobAssembler::ApplyStatus::kApplied,
             "Sequential diff must be applied")) {
    return false;
  }
  if (!Check(lob.LastUpdateId() == 102, "Sequence must advance to 102")) return false;
  if (!Check(!lob.bids().empty() && !lob.asks().empty(), "LOB must keep both sides")) {
    return false;
  }
  if (!Check(lob.bids().front().price.units == 10000,
             "Best bid price must remain 100.00")) {
    return false;
  }
  if (!Check(lob.asks().front().price.units == 10015,
             "Best ask should move to 100.15")) {
    return false;
  }

  CexLocalLobAssembler::DiffEvent diff2;
  diff2.first_update_id = 103;
  diff2.final_update_id = 103;
  diff2.bid_deltas = {Level(10005, 2, 800, 3)};  // new best bid
  if (!Check(lob.ApplyDiff(diff2) == CexLocalLobAssembler::ApplyStatus::kApplied,
             "Next sequential diff must be applied")) {
    return false;
  }
  if (!Check(lob.LastUpdateId() == 103, "Sequence must advance to 103")) return false;
  if (!Check(lob.bids().front().price.units == 10005,
             "Best bid should move to 100.05")) {
    return false;
  }

  return true;
}

bool TestOverlapAndOldDiffHandling() {
  CexLocalLobAssembler lob(10);
  if (!Check(lob.InitializeFromSnapshot(BaseSnapshot()), "Snapshot init must succeed")) {
    return false;
  }

  CexLocalLobAssembler::DiffEvent old;
  old.first_update_id = 90;
  old.final_update_id = 100;
  if (!Check(lob.ApplyDiff(old) == CexLocalLobAssembler::ApplyStatus::kIgnoredOld,
             "Old diff must be ignored")) {
    return false;
  }
  if (!Check(lob.LastUpdateId() == 100, "Old diff must not move sequence")) return false;

  CexLocalLobAssembler::DiffEvent overlap;
  overlap.first_update_id = 100;
  overlap.final_update_id = 101;
  overlap.bid_deltas = {Level(10000, 2, 1900, 3)};
  if (!Check(lob.ApplyDiff(overlap) == CexLocalLobAssembler::ApplyStatus::kApplied,
             "Overlapping diff covering expected id must apply")) {
    return false;
  }
  if (!Check(lob.LastUpdateId() == 101, "Sequence must move to 101")) return false;

  return true;
}

bool TestGapTriggersNeedReinit() {
  CexLocalLobAssembler lob(10);
  if (!Check(lob.InitializeFromSnapshot(BaseSnapshot()), "Snapshot init must succeed")) {
    return false;
  }

  CexLocalLobAssembler::DiffEvent gap;
  gap.first_update_id = 105;
  gap.final_update_id = 106;
  if (!Check(lob.ApplyDiff(gap) == CexLocalLobAssembler::ApplyStatus::kNeedReinit,
             "Gap in update IDs must trigger reset")) {
    return false;
  }
  if (!Check(!lob.IsInitialized(), "Assembler must be reset after gap")) return false;

  CexLocalLobAssembler::DiffEvent any_after_reset;
  any_after_reset.first_update_id = 107;
  any_after_reset.final_update_id = 107;
  if (!Check(lob.ApplyDiff(any_after_reset) == CexLocalLobAssembler::ApplyStatus::kNeedReinit,
             "Diff without re-init must request re-init")) {
    return false;
  }

  auto snapshot = BaseSnapshot();
  snapshot.last_update_id = 106;
  if (!Check(lob.InitializeFromSnapshot(snapshot), "Re-init from snapshot must succeed")) {
    return false;
  }
  if (!Check(lob.LastUpdateId() == 106, "Re-init must restore snapshot sequence")) {
    return false;
  }

  return true;
}

bool TestCrossedBookTriggersNeedReinit() {
  CexLocalLobAssembler lob(10);
  if (!Check(lob.InitializeFromSnapshot(BaseSnapshot()), "Snapshot init must succeed")) {
    return false;
  }

  CexLocalLobAssembler::DiffEvent crossed;
  crossed.first_update_id = 101;
  crossed.final_update_id = 101;
  crossed.bid_deltas = {Level(10020, 2, 1000, 3)};  // best bid >= best ask

  if (!Check(lob.ApplyDiff(crossed) == CexLocalLobAssembler::ApplyStatus::kNeedReinit,
             "Crossed book must trigger reset")) {
    return false;
  }
  if (!Check(!lob.IsInitialized(), "Crossed book should reset assembler state")) {
    return false;
  }

  return true;
}

bool TestConfigureMaxLevelsTrimsBook() {
  CexLocalLobAssembler lob(100);
  CexLocalLobAssembler::Snapshot snapshot;
  snapshot.last_update_id = 500;

  for (int i = 0; i < 50; ++i) {
    snapshot.bids.push_back(Level(10000 - i, 2, 1000 + i, 3));
    snapshot.asks.push_back(Level(10020 + i, 2, 1000 + i, 3));
  }

  if (!Check(lob.InitializeFromSnapshot(snapshot), "Large snapshot init must succeed")) {
    return false;
  }
  if (!Check(lob.bids().size() == 50 && lob.asks().size() == 50,
             "Initial size must match snapshot depth")) {
    return false;
  }

  lob.ConfigureMaxLevels(7);
  if (!Check(lob.bids().size() == 7 && lob.asks().size() == 7,
             "ConfigureMaxLevels must trim both sides")) {
    return false;
  }
  if (!Check(IsSortedDescByPrice(lob.bids()), "Trimmed bids must stay sorted desc")) return false;
  if (!Check(IsSortedAscByPrice(lob.asks()), "Trimmed asks must stay sorted asc")) return false;

  return true;
}

bool TestHighVolumeSequentialDiffLoadMaintainsTopOfBook() {
  CexLocalLobAssembler lob(25);
  if (!Check(lob.InitializeFromSnapshot(BaseSnapshot()), "Snapshot init must succeed")) {
    return false;
  }

  uint64_t update_id = 100;
  int64_t best_bid = 10000;
  int64_t best_ask = 10010;
  int64_t prev_bid = best_bid;
  int64_t prev_ask = best_ask;

  for (int i = 0; i < 400; ++i) {
    ++update_id;
    if ((i % 2) == 0) {
      prev_bid = best_bid;
      prev_ask = best_ask;
      ++best_bid;
      ++best_ask;
    }

    CexLocalLobAssembler::DiffEvent diff;
    diff.first_update_id = update_id;
    diff.final_update_id = update_id;
    diff.bid_deltas = {Level(best_bid, 2, 1200 + (i % 100), 3),
                       Level(best_bid - 20, 2, 500 + (i % 40), 3)};
    diff.ask_deltas = {Level(best_ask, 2, 1100 + (i % 80), 3),
                       Level(best_ask + 20, 2, 600 + (i % 40), 3)};
    if ((i % 2) == 0) {
      diff.bid_deltas.push_back(Level(prev_bid, 2, 0, 3));
      diff.ask_deltas.push_back(Level(prev_ask, 2, 0, 3));
    }

    if (!Check(lob.ApplyDiff(diff) == CexLocalLobAssembler::ApplyStatus::kApplied,
               "Sequential high-volume diff must be applied")) {
      return false;
    }
  }

  if (!Check(lob.LastUpdateId() == 500, "Last update id mismatch after load test")) return false;
  if (!Check(!lob.bids().empty() && !lob.asks().empty(),
             "LOB must remain non-empty after load test")) {
    return false;
  }
  if (!Check(lob.bids().size() <= 25 && lob.asks().size() <= 25,
             "LOB must respect configured max_levels")) {
    return false;
  }
  if (!Check(IsSortedDescByPrice(lob.bids()), "Bids must stay sorted desc")) return false;
  if (!Check(IsSortedAscByPrice(lob.asks()), "Asks must stay sorted asc")) return false;
  if (!Check(lob.bids().front().price.units < lob.asks().front().price.units,
             "Book must remain non-crossed after high load")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestInitializeAndApplySequentialDiffs() && ok;
  ok = TestOverlapAndOldDiffHandling() && ok;
  ok = TestGapTriggersNeedReinit() && ok;
  ok = TestCrossedBookTriggersNeedReinit() && ok;
  ok = TestConfigureMaxLevelsTrimsBook() && ok;
  ok = TestHighVolumeSequentialDiffLoadMaintainsTopOfBook() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] cex_local_lob_assembler_test" << std::endl;
  return EXIT_SUCCESS;
}
