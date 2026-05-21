#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "fob/common/v1/common.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "infra/simulated_venue_adapter.hpp"

namespace {

using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueFeedChannel;
using cex::venues::domain::VenueSnapshotRequest;
using cex::venues::domain::VenueSubscription;
using cex::venues::infra::SimulatedVenueAdapter;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

fob::common::v1::Instrument BtcUsdt() {
  fob::common::v1::Instrument instrument;
  instrument.set_symbol("BTC/USDT");
  instrument.set_base("BTC");
  instrument.set_quote("USDT");
  return instrument;
}

VenueSubscription DefaultSubscription() {
  VenueSubscription sub;
  sub.instrument = BtcUsdt();
  sub.venue_symbol = "BTCUSDT";
  sub.channels = {
      VenueFeedChannel::kOrderBook,
      VenueFeedChannel::kTrades,
      VenueFeedChannel::kTicker,
      VenueFeedChannel::kStatus,
  };
  sub.depth_levels = 10;
  return sub;
}

VenueSnapshotRequest DefaultRequest() {
  VenueSnapshotRequest request;
  request.instrument = BtcUsdt();
  request.venue_symbol = "BTCUSDT";
  request.depth_levels = 5;
  return request;
}

fob::execution::v1::ExecutionIntent DefaultIntent() {
  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("intent-1");
  intent.set_client_order_id("client-1");
  intent.set_venue("binance");
  *intent.mutable_instrument() = BtcUsdt();
  intent.set_venue_symbol("BTCUSDT");
  intent.mutable_target_qty()->set_units(2500);
  intent.mutable_target_qty()->set_scale(3);
  intent.mutable_limit_price()->set_units(10123);
  intent.mutable_limit_price()->set_scale(2);
  return intent;
}

bool TestConnectionLifecycle() {
  SimulatedVenueAdapter adapter("binance");

  const auto hb0 = adapter.Heartbeat();
  if (!Check(hb0.status == VenueConnectionStatus::kDisconnected,
             "Heartbeat before connect must be disconnected")) {
    return false;
  }

  if (!Check(!adapter.RequestSnapshot(DefaultRequest()).has_value(),
             "RequestSnapshot must fail before connect")) {
    return false;
  }

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.Subscribe({DefaultSubscription()}),
             "Subscribe must succeed after connect")) {
    return false;
  }

  const auto hb1 = adapter.Heartbeat();
  if (!Check(hb1.status == VenueConnectionStatus::kConnected,
             "Heartbeat after connect must be connected")) {
    return false;
  }

  if (!Check(adapter.Reconnect(), "Reconnect must succeed")) return false;
  const auto hb2 = adapter.Heartbeat();
  if (!Check(hb2.reconnect_attempts == 1,
             "Reconnect attempts counter must increase")) {
    return false;
  }

  return true;
}

bool TestSnapshotContainsRequiredFields() {
  SimulatedVenueAdapter adapter("binance");
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.Subscribe({DefaultSubscription()}), "Subscribe must succeed")) {
    return false;
  }

  const auto snapshot = adapter.RequestSnapshot(DefaultRequest());
  if (!Check(snapshot.has_value(), "Snapshot must be produced")) return false;

  if (!Check(snapshot->status == VenueConnectionStatus::kConnected,
             "Snapshot status must be connected")) {
    return false;
  }
  if (!Check(snapshot->instrument.symbol() == "BTC/USDT",
             "Snapshot symbol must be BTC/USDT")) {
    return false;
  }
  if (!Check(snapshot->bids.size() == 5 && snapshot->asks.size() == 5,
             "Snapshot must contain requested depth")) {
    return false;
  }
  if (!Check(snapshot->best_bid.units > 0 && snapshot->best_ask.units > 0,
             "Snapshot must have BBO")) {
    return false;
  }
  if (!Check(snapshot->best_bid.units < snapshot->best_ask.units,
             "Best bid must be less than best ask")) {
    return false;
  }
  if (!Check(snapshot->spread.units > 0, "Spread must be positive")) {
    return false;
  }
  if (!Check(snapshot->fees.maker.units > 0 && snapshot->fees.taker.units > 0,
             "Fees must be populated")) {
    return false;
  }
  if (!Check(snapshot->trading_rules.tick_size.units > 0 &&
                 snapshot->trading_rules.lot_size.units > 0,
             "Tick and lot size must be populated")) {
    return false;
  }
  if (!Check(snapshot->volume_24h.units > 0, "Volume24h must be populated")) {
    return false;
  }
  if (!Check(!snapshot->empty(), "Snapshot must not be empty")) return false;

  return true;
}

bool TestSendOrder() {
  SimulatedVenueAdapter adapter("binance");
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  const auto result = adapter.SendOrder(DefaultIntent());
  if (!Check(result.accepted, "Connected adapter must accept order")) return false;
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "Order status must be filled")) {
    return false;
  }
  if (!Check(result.filled_qty.units == 2500 && result.filled_qty.scale == 3,
             "Filled qty must match target qty")) {
    return false;
  }
  if (!Check(result.remaining_qty.units == 0,
             "Remaining qty must be zero for simulated fill")) {
    return false;
  }
  if (!Check(result.average_price.units == 10123 && result.average_price.scale == 2,
             "Average price must follow limit price when provided")) {
    return false;
  }
  if (!Check(!result.venue_order_id.empty(), "Venue order id must be set")) {
    return false;
  }

  return true;
}

bool TestSendOrderRejectedWhenDisconnected() {
  SimulatedVenueAdapter adapter("binance");

  const auto result = adapter.SendOrder(DefaultIntent());
  if (!Check(!result.accepted, "Disconnected adapter must reject order")) return false;
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "Disconnected order must be rejected")) {
    return false;
  }
  if (!Check(result.error_code == "VENUE_DISCONNECTED",
             "Disconnected order must return VENUE_DISCONNECTED code")) {
    return false;
  }

  return true;
}

bool TestDepthDefaultInstrumentFallbackAndSequenceGrowth() {
  SimulatedVenueAdapter adapter("binance");
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.Subscribe({DefaultSubscription()}), "Subscribe must succeed")) return false;

  VenueSnapshotRequest req;
  req.depth_levels = 0;  // must fallback to adapter default depth=20

  const auto snap1 = adapter.RequestSnapshot(req);
  if (!Check(snap1.has_value(), "Snapshot with empty request must be produced")) return false;
  if (!Check(snap1->bids.size() == 20 && snap1->asks.size() == 20,
             "depth_levels=0 must fallback to 20 levels")) {
    return false;
  }
  if (!Check(snap1->instrument.symbol() == "BTC/USDT",
             "Instrument must fallback from subscription")) {
    return false;
  }
  if (!Check(snap1->venue_symbol == "BTCUSDT",
             "Venue symbol must fallback from subscription")) {
    return false;
  }

  const auto snap2 = adapter.RequestSnapshot(req);
  if (!Check(snap2.has_value(), "Second snapshot must be produced")) return false;
  if (!Check(snap2->sequence > snap1->sequence, "Sequence must strictly increase")) {
    return false;
  }
  if (!Check(snap2->best_bid.units != snap1->best_bid.units ||
                 snap2->best_ask.units != snap1->best_ask.units,
             "Synthetic market should move between snapshots")) {
    return false;
  }

  return true;
}

bool TestSendOrderWithoutLimitPriceUsesMarketMid() {
  SimulatedVenueAdapter adapter("binance");
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  fob::execution::v1::ExecutionIntent intent = DefaultIntent();
  intent.clear_limit_price();

  const auto result = adapter.SendOrder(intent);
  if (!Check(result.accepted, "Market order must be accepted on connected adapter")) {
    return false;
  }
  if (!Check(result.average_price.units > 0, "Market order must have synthetic average price")) {
    return false;
  }
  if (!Check(result.average_price.scale == 2,
             "Market average price scale must follow adapter price scale")) {
    return false;
  }

  return true;
}

bool TestVenueTypeCanRepresentDexOrAmm() {
  SimulatedVenueAdapter dex_adapter("uniswap_v3", cex::venues::domain::VenueType::kDex);
  if (!Check(dex_adapter.Type() == cex::venues::domain::VenueType::kDex,
             "Explicit adapter type kDex must be returned")) {
    return false;
  }

  SimulatedVenueAdapter amm_adapter("curve_pool", cex::venues::domain::VenueType::kAmm);
  if (!Check(amm_adapter.Type() == cex::venues::domain::VenueType::kAmm,
             "Explicit adapter type kAmm must be returned")) {
    return false;
  }

  return true;
}

bool TestVenueProfilesProduceDistinctMarkets() {
  SimulatedVenueAdapter binance("binance");
  SimulatedVenueAdapter coinbase("coinbase");
  SimulatedVenueAdapter uniswap("uniswap_v3", cex::venues::domain::VenueType::kDex);

  if (!Check(binance.Connect() && coinbase.Connect() && uniswap.Connect(),
             "All adapters must connect")) {
    return false;
  }

  if (!Check(binance.Subscribe({DefaultSubscription()}) &&
                 coinbase.Subscribe({DefaultSubscription()}) &&
                 uniswap.Subscribe({DefaultSubscription()}),
             "All adapters must subscribe")) {
    return false;
  }

  const auto binance_snapshot = binance.RequestSnapshot(DefaultRequest());
  const auto coinbase_snapshot = coinbase.RequestSnapshot(DefaultRequest());
  const auto uniswap_snapshot = uniswap.RequestSnapshot(DefaultRequest());
  if (!Check(binance_snapshot.has_value() &&
                 coinbase_snapshot.has_value() &&
                 uniswap_snapshot.has_value(),
             "All venue profiles must produce snapshots")) {
    return false;
  }

  if (!Check(binance_snapshot->mid_price.units > 6800000 &&
                 coinbase_snapshot->mid_price.units > 6800000 &&
                 uniswap_snapshot->mid_price.units > 6800000,
             "All simulated venues must publish realistic BTC mids")) {
    return false;
  }

  if (!Check(binance_snapshot->mid_price.units != coinbase_snapshot->mid_price.units &&
                 coinbase_snapshot->mid_price.units != uniswap_snapshot->mid_price.units,
             "Venue mid prices must differ across venues")) {
    return false;
  }

  if (!Check(binance_snapshot->volume_24h.units != coinbase_snapshot->volume_24h.units &&
                 coinbase_snapshot->volume_24h.units != uniswap_snapshot->volume_24h.units,
             "Venue 24h volumes must differ across venues")) {
    return false;
  }

  if (!Check(uniswap_snapshot->spread.units > binance_snapshot->spread.units,
             "DEX profile must keep a wider spread than Binance")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestConnectionLifecycle() && ok;
  ok = TestSnapshotContainsRequiredFields() && ok;
  ok = TestSendOrder() && ok;
  ok = TestSendOrderRejectedWhenDisconnected() && ok;
  ok = TestDepthDefaultInstrumentFallbackAndSequenceGrowth() && ok;
  ok = TestSendOrderWithoutLimitPriceUsesMarketMid() && ok;
  ok = TestVenueTypeCanRepresentDexOrAmm() && ok;
  ok = TestVenueProfilesProduceDistinctMarkets() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] simulated_venue_adapter_test" << std::endl;
  return EXIT_SUCCESS;
}
