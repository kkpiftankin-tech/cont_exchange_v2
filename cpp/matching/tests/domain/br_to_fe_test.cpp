#include <cassert>

#include "cex/common/decimal.hpp"
#include "domain/batch_result_to_fill_events.hpp"
#include "fob/matching/v1/batch.pb.h"

using cex::common::Decimal;

namespace {

fob::matching::v1::BatchResult FakeBatchResult() {
  fob::matching::v1::BatchResult res;
  res.set_batch_id("batch_id");

  auto* internal = res.add_fills();
  internal->set_order_id("order_id");
  internal->mutable_executed_qty()->CopyFrom(Decimal{1, 2}.to_proto());
  internal->mutable_executed_notional()->CopyFrom(Decimal{3, 4}.to_proto());
  internal->set_liquidity_source("internal");
  internal->mutable_provenance()->set_liquidity_source("internal");

  auto* external = res.add_fills();
  external->set_order_id("order_id");
  external->mutable_executed_qty()->CopyFrom(Decimal{5, 2}.to_proto());
  external->mutable_executed_notional()->CopyFrom(Decimal{7, 4}.to_proto());
  external->set_liquidity_source("cex_hedge");
  external->mutable_provenance()->set_liquidity_source("cex_hedge");
  external->mutable_provenance()->set_venue_id("binance");
  external->mutable_provenance()->set_snapshot_id("snapshot-1");
  external->mutable_provenance()->set_curve_id("curve-1");

  auto* external_same_source = res.add_fills();
  external_same_source->set_order_id("order_id");
  external_same_source->mutable_executed_qty()->CopyFrom(Decimal{9, 2}.to_proto());
  external_same_source->mutable_executed_notional()->CopyFrom(Decimal{11, 4}.to_proto());
  external_same_source->set_liquidity_source("cex_hedge");
  external_same_source->mutable_provenance()->set_liquidity_source("cex_hedge");
  external_same_source->mutable_provenance()->set_venue_id("coinbase");
  external_same_source->mutable_provenance()->set_snapshot_id("snapshot-2");
  external_same_source->mutable_provenance()->set_curve_id("curve-2");

  return res;
}

}  // namespace

int main() {
  auto fill_events = cex::matching::domain::BatchResultToFillEvents(FakeBatchResult());

  assert(fill_events.size() == 3);

  bool saw_internal = false;
  bool saw_binance = false;
  bool saw_coinbase = false;
  for (const auto& event : fill_events) {
    assert(event.batch_id() == "batch_id");
    assert(event.order_id() == "order_id");

    if (event.liquidity_source() == "internal") {
      saw_internal = true;
      assert(Decimal::cmp(Decimal{1, 2}, Decimal::from_proto(event.exec_qty())) == 0);
      assert(Decimal::cmp(Decimal{3, 4}, Decimal::from_proto(event.exec_price())) == 0);
      assert(event.provenance().liquidity_source() == "internal");
    }
    if (event.liquidity_source() == "cex_hedge" &&
        event.provenance().venue_id() == "binance") {
      saw_binance = true;
      assert(Decimal::cmp(Decimal{5, 2}, Decimal::from_proto(event.exec_qty())) == 0);
      assert(Decimal::cmp(Decimal{7, 4}, Decimal::from_proto(event.exec_price())) == 0);
      assert(event.provenance().snapshot_id() == "snapshot-1");
      assert(event.provenance().curve_id() == "curve-1");
    }
    if (event.liquidity_source() == "cex_hedge" &&
        event.provenance().venue_id() == "coinbase") {
      saw_coinbase = true;
      assert(Decimal::cmp(Decimal{9, 2}, Decimal::from_proto(event.exec_qty())) == 0);
      assert(Decimal::cmp(Decimal{11, 4}, Decimal::from_proto(event.exec_price())) == 0);
      assert(event.provenance().snapshot_id() == "snapshot-2");
      assert(event.provenance().curve_id() == "curve-2");
    }
  }

  assert(saw_internal);
  assert(saw_binance);
  assert(saw_coinbase);
  return 0;
}
