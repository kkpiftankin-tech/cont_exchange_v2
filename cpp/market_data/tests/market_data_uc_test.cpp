#include <cstdlib>
#include <iostream>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "cex/common/defer.hpp"

#include "app/market_data_uc.hpp"
#include "domain/ports/i_order_book_storage.hpp"
#include "infra/order_book_channel.hpp"

namespace {

struct FakeBatchStorage final : public cex::market_data::app::IBatchOutputsStorage {
  int save_batch_calls{0};
  int save_fills_calls{0};
  std::string last_batch_id;

  bool SaveBatchResult(const fob::matching::v1::BatchResult& evt) override {
    ++save_batch_calls;
    last_batch_id = evt.batch_id();
    return true;
  }

  bool SaveFills(const fob::matching::v1::BatchResult& evt) override {
    ++save_fills_calls;
    last_batch_id = evt.batch_id();
    return true;
  }

  int save_group_calls{0};
  bool SaveExecutionGroup(const fob::matching::v1::ExecutionGroup& evt) override {
    ++save_group_calls;
    last_batch_id = evt.batch_id();
    return true;
  }
};

struct FakeOrderBookStorage final : public cex::market_data::domain::IOrderBookStorage {
  void Store(const cex::market_data::domain::OrderBook&) override {}
};

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

void SetDecimal(fob::common::v1::Decimal* d, int64_t units, int32_t scale = 0) {
  d->set_units(units);
  d->set_scale(scale);
}

fob::venue::v1::VenueSnapshot MakeVenueSnapshot(const std::string& venue,
                                                const std::string& symbol,
                                                int64_t bid_price,
                                                int64_t bid_qty,
                                                int64_t ask_price,
                                                int64_t ask_qty) {
  fob::venue::v1::VenueSnapshot snap;
  snap.set_venue_id(venue);
  snap.mutable_instrument()->set_symbol(symbol);
  snap.set_status("connected");
  snap.mutable_timestamp()->set_seconds(1);
  SetDecimal(snap.add_bid_prices(), bid_price);
  SetDecimal(snap.add_bid_quantities(), bid_qty);
  SetDecimal(snap.add_ask_prices(), ask_price);
  SetDecimal(snap.add_ask_quantities(), ask_qty);
  return snap;
}

}  // namespace

int main() {
  FakeBatchStorage storage;
  FakeOrderBookStorage storage2;

  cex::market_data::infra::OrderBookChannel channel;
  auto _ = cex::common::Defer([&channel] { channel.Close(); });

  cex::market_data::app::MarketDataUseCases uc(
      &storage, nullptr, &storage2, &channel);

  fob::marketdata::v1::MarketDataRaw raw;
  auto* ticker = raw.mutable_ticker();
  ticker->set_venue("sim");
  ticker->mutable_instrument()->set_symbol("BTC/USDT");
  ticker->mutable_last()->set_units(10123);
  ticker->mutable_last()->set_scale(2);
  uc.OnMarketDataRaw(raw);

  auto saved_ticker = uc.GetLastTicker("sim", "BTC/USDT");
  if (!Check(saved_ticker.has_value(), "ticker must be stored in memory")) return EXIT_FAILURE;
  if (!Check(saved_ticker->last().units() == 10123, "stored ticker must contain same last units")) {
    return EXIT_FAILURE;
  }

  fob::matching::v1::BatchResult batch;
  boost::uuids::random_generator gen;

  auto batch_id = boost::uuids::to_string(gen());
  batch.set_batch_id(batch_id);
  auto* fill1 = batch.add_fills();
  fill1->set_order_id(boost::uuids::to_string(gen()));
  fill1->set_user_id(boost::uuids::to_string(gen()));

  auto* fill2 = batch.add_fills();
  fill2->set_order_id(boost::uuids::to_string(gen()));
  fill2->set_user_id(boost::uuids::to_string(gen()));

  uc.OnBatchResult(batch);

  if (!Check(storage.save_batch_calls == 1, "SaveBatchResult must be called exactly once")) {
    return EXIT_FAILURE;
  }
  if (!Check(storage.save_fills_calls == 1, "SaveFills must be called exactly once")) {
    return EXIT_FAILURE;
  }
  if (!Check(storage.last_batch_id == batch_id, "batch id must be passed to storage")) {
    return EXIT_FAILURE;
  }

  // F-09 observability: grouped combo execution → SaveExecutionGroup.
  fob::matching::v1::ExecutionGroup eg;
  const auto eg_batch_id = boost::uuids::to_string(gen());
  eg.set_execution_group_id(boost::uuids::to_string(gen()));
  eg.set_parent_order_id(boost::uuids::to_string(gen()));
  eg.set_batch_id(eg_batch_id);
  eg.add_leg_results()->set_leg_id(boost::uuids::to_string(gen()));
  eg.add_leg_results()->set_leg_id(boost::uuids::to_string(gen()));
  uc.OnExecutionGroup(eg);
  if (!Check(storage.save_group_calls == 1, "SaveExecutionGroup must be called exactly once")) {
    return EXIT_FAILURE;
  }
  if (!Check(storage.last_batch_id == eg_batch_id, "execution group batch id must reach storage")) {
    return EXIT_FAILURE;
  }

  auto stats = uc.GetBatchOutputsStats();
  if (!Check(stats.batches_processed == 1, "batches_processed must increase")) {
    return EXIT_FAILURE;
  }
  if (!Check(stats.fills_processed == 2, "fills_processed must equal number of fills")) {
    return EXIT_FAILURE;
  }
  if (!Check(stats.last_batch_id == batch_id, "last_batch_id must be updated")) {
    return EXIT_FAILURE;
  }

  // Drain earlier publications from marketdata.raw + batch.outputs paths.
  (void)channel.Consume();
  (void)channel.Consume();

  uc.OnVenueSnapshot(MakeVenueSnapshot("binance", "BTC/USDT", 100, 2, 101, 3));
  auto ob1 = channel.Consume();
  if (!Check(ob1.has_value(), "first venue snapshot must publish order book")) {
    return EXIT_FAILURE;
  }
  if (!(ob1->BestBid() == 100.0 && ob1->BestAsk() == 101.0)) {
    std::cerr << "[FAIL] single venue BBO must match source snapshot: bestBid="
              << ob1->BestBid() << " bestAsk=" << ob1->BestAsk() << std::endl;
    return EXIT_FAILURE;
  }

  uc.OnVenueSnapshot(MakeVenueSnapshot("coinbase", "BTC/USDT", 102, 1, 103, 2));
  auto ob2 = channel.Consume();
  if (!Check(ob2.has_value(), "second venue snapshot must publish aggregated order book")) {
    return EXIT_FAILURE;
  }
  if (!Check(ob2->BestBid() == 102.0, "aggregated best bid must be max bid across venues")) {
    return EXIT_FAILURE;
  }
  if (!Check(ob2->BestAsk() == 101.0, "aggregated best ask must be min ask across venues")) {
    return EXIT_FAILURE;
  }

  std::cout << "[OK] market_data_uc_test passed" << std::endl;
  return EXIT_SUCCESS;
}
