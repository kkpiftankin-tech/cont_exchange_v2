// ============================================================================
// combo_external_routing_test.cpp — F-09 MVP-5 (ADR-037). Hand-rolled harness.
// SplitInternalExternal + BuildExternalIntent (side/qty/venues/linkage).
// ============================================================================

#include <iostream>

#include "app/combo_external_routing.hpp"

namespace {
using cex::common::Decimal;
namespace d = cex::matching::domain;
namespace a = cex::matching::app;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

d::VectorLeg Leg(const std::string& id, const std::string& sym, std::int64_t ratio,
                 std::int64_t q_max, std::int64_t filled, std::vector<std::string> venues) {
  d::VectorLeg l;
  l.leg_id = id;
  l.instrument_symbol = sym;
  l.target_ratio = Decimal{ratio, 0};
  l.q_max = Decimal{q_max, 0};
  l.filled_cum = Decimal{filled, 0};
  l.venue_preferences = std::move(venues);
  return l;
}
}  // namespace

int main() {
  bool ok = true;

  // is_external semantics.
  ok = expect(!Leg("a", "BTC", 1, 10, 0, {}).is_external(), "empty venues → internal") && ok;
  ok = expect(!Leg("a", "BTC", 1, 10, 0, {"internal"}).is_external(), "'internal' → internal") && ok;
  ok = expect(Leg("a", "BTC", 1, 10, 0, {"binance"}).is_external(), "venue → external") && ok;

  // Split.
  d::MultiLegVectorOrder order;
  order.parent_order_id = "p1";
  order.legs.push_back(Leg("L1", "BTCUSDT", 1, 10, 0, {}));            // internal
  order.legs.push_back(Leg("L2", "ETHUSDT", 1, 10, 0, {"internal"}));  // internal
  order.legs.push_back(Leg("L3", "SOLUSDT", -1, 10, 3, {"binance", "kraken"}));  // external sell
  const auto split = a::SplitInternalExternal(order);
  ok = expect(split.internal.size() == 2, "split: 2 internal") && ok;
  ok = expect(split.external.size() == 1 && split.external[0].leg_id == "L3", "split: 1 external (L3)") && ok;

  // BuildExternalIntent for L3 (sell, remaining 7, two venues).
  const auto intent = a::BuildExternalIntent("p1", "batch-9", "intent-7", split.external[0]);
  ok = expect(intent.intent_id() == "intent-7" && intent.batch_id() == "batch-9", "intent ids") && ok;
  ok = expect(intent.internal_order_id() == "L3", "internal_order_id = leg_id (linkage)") && ok;
  ok = expect(intent.instrument().symbol() == "SOLUSDT", "intent symbol") && ok;
  ok = expect(intent.side() == fob::common::v1::SIDE_SELL, "side SELL from negative ratio") && ok;
  ok = expect(intent.target_qty().units() == 7 && intent.target_qty().scale() == 0,
              "target_qty = remaining (10-3=7)") && ok;
  ok = expect(intent.allowed_venues_size() == 2, "allowed_venues = venue_preferences") && ok;
  // T-F09-068: discrete order targets a concrete venue (primary non-internal pref).
  ok = expect(intent.venue() == "binance", "intent.venue = primary venue_preference") && ok;

  if (ok) { std::cout << "combo_external_routing_test: ALL PASSED\n"; return 0; }
  return 1;
}
