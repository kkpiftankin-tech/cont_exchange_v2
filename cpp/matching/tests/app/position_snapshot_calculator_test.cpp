#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "app/position_snapshot_calculator.hpp"

namespace {

using cex::common::Decimal;
using cex::matching::app::PositionSnapshot;
using cex::matching::app::PositionSnapshotCalculator;

fob::common::v1::Decimal Dec(const int64_t units, const int32_t scale = 0) {
  fob::common::v1::Decimal out;
  out.set_units(units);
  out.set_scale(scale);
  return out;
}

bool Expect(const bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

void AddFill(fob::matching::v1::BatchResult* batch,
             const std::string& provider_id,
             const std::string& symbol,
             const fob::common::v1::Side side,
             const int64_t qty_units,
             const int32_t qty_scale = 0) {
  auto* fill = batch->add_fills();
  fill->set_user_id(provider_id);
  fill->mutable_instrument()->set_symbol(symbol);
  fill->set_side(side);
  *fill->mutable_executed_qty() = Dec(qty_units, qty_scale);
}

const PositionSnapshot* FindSnapshot(
    const std::vector<PositionSnapshot>& snapshots,
    const std::string& provider_id,
    const std::string& symbol) {
  for (const auto& snapshot : snapshots) {
    if (snapshot.provider_id == provider_id && snapshot.symbol == symbol) {
      return &snapshot;
    }
  }
  return nullptr;
}

fob::matching::v1::BatchResult MakeBaseBatch() {
  fob::matching::v1::BatchResult batch;
  batch.set_batch_id("batch-ps");
  batch.mutable_timestamp()->set_seconds(1700000000);
  batch.mutable_timestamp()->set_nanos(123000000);
  return batch;
}

bool TestBuyFillProducesPositiveNet() {
  PositionSnapshotCalculator calculator;
  auto batch = MakeBaseBatch();
  AddFill(&batch, "provider-a", "BTC/USDT", fob::common::v1::SIDE_BUY, 5, 0);

  const auto snapshots = calculator.Calculate(batch);
  const auto* snapshot = FindSnapshot(snapshots, "provider-a", "BTC/USDT");

  bool ok = true;
  ok = Expect(snapshot != nullptr, "BUY fill snapshot exists") && ok;
  if (snapshot != nullptr) {
    ok = Expect(snapshot->net_qty.units == 5, "BUY fill netQty is positive") && ok;
  }
  return ok;
}

bool TestSellFillProducesNegativeNet() {
  PositionSnapshotCalculator calculator;
  auto batch = MakeBaseBatch();
  AddFill(&batch, "provider-a", "BTC/USDT", fob::common::v1::SIDE_SELL, 7, 0);

  const auto snapshots = calculator.Calculate(batch);
  const auto* snapshot = FindSnapshot(snapshots, "provider-a", "BTC/USDT");

  bool ok = true;
  ok = Expect(snapshot != nullptr, "SELL fill snapshot exists") && ok;
  if (snapshot != nullptr) {
    ok = Expect(snapshot->net_qty.units == -7, "SELL fill netQty is negative") && ok;
  }
  return ok;
}

bool TestAggregatesMultipleFillsPerProviderSymbol() {
  PositionSnapshotCalculator calculator;
  auto batch = MakeBaseBatch();
  AddFill(&batch, "provider-a", "BTC/USDT", fob::common::v1::SIDE_BUY, 3, 0);
  AddFill(&batch, "provider-a", "BTC/USDT", fob::common::v1::SIDE_SELL, 1, 0);
  AddFill(&batch, "provider-a", "BTC/USDT", fob::common::v1::SIDE_BUY, 2, 0);

  const auto snapshots = calculator.Calculate(batch);
  const auto* snapshot = FindSnapshot(snapshots, "provider-a", "BTC/USDT");

  bool ok = true;
  ok = Expect(snapshot != nullptr, "aggregate snapshot exists") && ok;
  ok = Expect(snapshots.size() == 1, "same provider+symbol aggregated to one snapshot") && ok;
  if (snapshot != nullptr) {
    ok = Expect(snapshot->net_qty.units == 4, "aggregated netQty is signed sum") && ok;
  }
  return ok;
}

bool TestDoesNotMixProviderIds() {
  PositionSnapshotCalculator calculator;
  auto batch = MakeBaseBatch();
  AddFill(&batch, "provider-a", "BTC/USDT", fob::common::v1::SIDE_BUY, 2, 0);
  AddFill(&batch, "provider-b", "BTC/USDT", fob::common::v1::SIDE_BUY, 3, 0);

  const auto snapshots = calculator.Calculate(batch);
  const auto* a = FindSnapshot(snapshots, "provider-a", "BTC/USDT");
  const auto* b = FindSnapshot(snapshots, "provider-b", "BTC/USDT");

  bool ok = true;
  ok = Expect(snapshots.size() == 2, "different providers produce separate snapshots") && ok;
  ok = Expect(a != nullptr, "provider-a snapshot exists") && ok;
  ok = Expect(b != nullptr, "provider-b snapshot exists") && ok;
  return ok;
}

bool TestDoesNotMixSymbols() {
  PositionSnapshotCalculator calculator;
  auto batch = MakeBaseBatch();
  AddFill(&batch, "provider-a", "BTC/USDT", fob::common::v1::SIDE_BUY, 2, 0);
  AddFill(&batch, "provider-a", "ETH/USDT", fob::common::v1::SIDE_BUY, 3, 0);

  const auto snapshots = calculator.Calculate(batch);
  const auto* btc = FindSnapshot(snapshots, "provider-a", "BTC/USDT");
  const auto* eth = FindSnapshot(snapshots, "provider-a", "ETH/USDT");

  bool ok = true;
  ok = Expect(snapshots.size() == 2, "different symbols produce separate snapshots") && ok;
  ok = Expect(btc != nullptr, "BTC snapshot exists") && ok;
  ok = Expect(eth != nullptr, "ETH snapshot exists") && ok;
  return ok;
}

bool TestCopiesClearingPriceBatchIdAndTimestamp() {
  PositionSnapshotCalculator calculator;
  auto batch = MakeBaseBatch();
  AddFill(&batch, "provider-a", "BTC/USDT", fob::common::v1::SIDE_BUY, 2, 0);
  (*batch.mutable_clear_prices())["BTC/USDT"] = Dec(10025, 2);

  const auto snapshots = calculator.Calculate(batch);
  const auto* snapshot = FindSnapshot(snapshots, "provider-a", "BTC/USDT");

  bool ok = true;
  ok = Expect(snapshot != nullptr, "snapshot exists for metadata copy") && ok;
  if (snapshot != nullptr) {
    ok = Expect(snapshot->batch_id == "batch-ps", "batch_id copied from BatchResult") && ok;
    ok = Expect(snapshot->clearing_price.units() == 10025,
                "clearingPrice copied from clear_prices") && ok;
    ok = Expect(snapshot->clearing_price.scale() == 2,
                "clearingPrice scale copied from clear_prices") && ok;
    ok = Expect(snapshot->timestamp.seconds() == 1700000000,
                "timestamp seconds copied from BatchResult") && ok;
    ok = Expect(snapshot->timestamp.nanos() == 123000000,
                "timestamp nanos copied from BatchResult") && ok;
  }
  return ok;
}

bool TestSkipsZeroNetAndInvalidFills() {
  PositionSnapshotCalculator calculator;
  auto batch = MakeBaseBatch();
  AddFill(&batch, "provider-a", "BTC/USDT", fob::common::v1::SIDE_BUY, 3, 0);
  AddFill(&batch, "provider-a", "BTC/USDT", fob::common::v1::SIDE_SELL, 3, 0);

  auto* invalid_missing_provider = batch.add_fills();
  invalid_missing_provider->mutable_instrument()->set_symbol("ETH/USDT");
  invalid_missing_provider->set_side(fob::common::v1::SIDE_BUY);
  *invalid_missing_provider->mutable_executed_qty() = Dec(5, 0);

  auto* invalid_missing_symbol = batch.add_fills();
  invalid_missing_symbol->set_user_id("provider-b");
  invalid_missing_symbol->set_side(fob::common::v1::SIDE_BUY);
  *invalid_missing_symbol->mutable_executed_qty() = Dec(5, 0);

  auto* invalid_zero_qty = batch.add_fills();
  invalid_zero_qty->set_user_id("provider-c");
  invalid_zero_qty->mutable_instrument()->set_symbol("ETH/USDT");
  invalid_zero_qty->set_side(fob::common::v1::SIDE_BUY);
  *invalid_zero_qty->mutable_executed_qty() = Dec(0, 0);

  auto* invalid_side = batch.add_fills();
  invalid_side->set_user_id("provider-d");
  invalid_side->mutable_instrument()->set_symbol("ETH/USDT");
  invalid_side->set_side(fob::common::v1::SIDE_UNSPECIFIED);
  *invalid_side->mutable_executed_qty() = Dec(5, 0);

  const auto snapshots = calculator.Calculate(batch);

  bool ok = true;
  ok = Expect(snapshots.empty(), "zero-net and invalid fills are omitted") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestBuyFillProducesPositiveNet() && ok;
  ok = TestSellFillProducesNegativeNet() && ok;
  ok = TestAggregatesMultipleFillsPerProviderSymbol() && ok;
  ok = TestDoesNotMixProviderIds() && ok;
  ok = TestDoesNotMixSymbols() && ok;
  ok = TestCopiesClearingPriceBatchIdAndTimestamp() && ok;
  ok = TestSkipsZeroNetAndInvalidFills() && ok;
  return ok ? 0 : 1;
}
