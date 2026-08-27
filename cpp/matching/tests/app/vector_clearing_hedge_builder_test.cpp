// ============================================================================
// vector_clearing_hedge_builder_test.cpp — F-05A (T-F05A-305, ADR-049).
// Hand-rolled. VectorClearingInput + outcome → ExecutionIntent[] (F-12 hedge).
// ============================================================================

#include <iostream>
#include <string>

#include "app/vector_clearing_hedge_builder.hpp"
#include "app/vector_clearing_use_case.hpp"

namespace {

using cex::common::Decimal;
namespace app = cex::matching::app;
namespace ev1 = fob::execution::v1;
namespace mv1 = fob::marketdata::v1;
namespace cv1 = fob::common::v1;

int g_failures = 0;
bool expect(bool cond, const std::string& msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; ++g_failures; }
  return cond;
}

void SetDec(cv1::Decimal* d, std::int64_t units, std::int32_t scale = 0) {
  d->set_units(units);
  d->set_scale(scale);
}

mv1::VectorFlowSegment* AddSeg(mv1::VectorClearingInput* in, const std::string& id,
                               const std::string& venue, const std::string& pair,
                               mv1::VectorLevelSide side, std::int64_t eff_price) {
  auto* s = in->add_segments();
  s->set_segment_id(id);
  s->set_source_order_id("ord-" + id);
  s->set_venue_id(venue);
  s->set_pair(pair);
  s->set_side(side);
  SetDec(s->mutable_effective_price(), eff_price);
  return s;
}

void TestBuild() {
  mv1::VectorClearingInput in;
  in.set_batch_id("b1");
  AddSeg(&in, "s0", "vx", "BTC/USDT", mv1::VECTOR_LEVEL_SIDE_BID, 100);
  AddSeg(&in, "s1", "vy", "ETH/USDT", mv1::VECTOR_LEVEL_SIDE_ASK, 50);

  app::VectorClearingOutcome o;
  o.solve.x = {Decimal{2, 0}, Decimal{1, 0}};  // оба > 0

  auto intents = app::BuildHedgeIntents(in, o);
  expect(intents.size() == 2, "2 intents (both x>0)");

  const auto& i0 = intents[0];
  expect(i0.intent_id() == "b1|s0", "intent_id = batch|segment");
  expect(i0.batch_id() == "b1", "batch_id");
  expect(i0.venue() == "vx", "venue = segment venue");
  expect(i0.instrument().symbol() == "BTC/USDT" && i0.instrument().base() == "BTC" &&
             i0.instrument().quote() == "USDT",
         "instrument parsed from pair");
  expect(i0.side() == cv1::SIDE_SELL, "bid segment → SELL (against their bid)");
  expect(i0.target_qty().units() == 2, "target_qty = x_0 = 2");
  expect(i0.limit_price().units() == 100, "limit_price = effective_price");
  expect(i0.reason() == "f05a_vector_clearing", "reason");
  expect(i0.source() == ev1::HEDGE_SOURCE_AUTO_BATCH, "source AUTO_BATCH");
  expect(i0.internal_order_id() == "ord-s0", "source-trace internal_order_id");

  const auto& i1 = intents[1];
  expect(i1.side() == cv1::SIDE_BUY, "ask segment → BUY");
  expect(i1.venue() == "vy" && i1.target_qty().units() == 1, "seg1 venue+qty");
}

void TestSkipsZeroX() {
  mv1::VectorClearingInput in;
  in.set_batch_id("b2");
  AddSeg(&in, "s0", "vx", "BTC/USDT", mv1::VECTOR_LEVEL_SIDE_BID, 100);
  AddSeg(&in, "s1", "vy", "ETH/USDT", mv1::VECTOR_LEVEL_SIDE_ASK, 50);

  app::VectorClearingOutcome o;
  o.solve.x = {Decimal{3, 0}, Decimal{0, 0}};  // s1 не исполняется

  auto intents = app::BuildHedgeIntents(in, o);
  expect(intents.size() == 1 && intents[0].intent_id() == "b2|s0",
         "skip x_i <= 0 (only s0)");
}

}  // namespace

int main() {
  TestBuild();
  TestSkipsZeroX();
  if (g_failures == 0) {
    std::cout << "vector_clearing_hedge_builder_test: ALL PASSED\n";
    return 0;
  }
  std::cerr << "vector_clearing_hedge_builder_test: " << g_failures << " FAILURE(S)\n";
  return 1;
}
