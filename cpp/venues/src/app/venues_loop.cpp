#include "app/venues_loop.hpp"

#include <chrono>

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "cex/common/decimal.hpp"

#include "fob/marketdata/v1/marketdata_raw.pb.h"
#include "fob/execution/v1/execution.pb.h"

namespace cex::venues::app {

using cex::common::Decimal;

static fob::common::v1::Decimal dec_from_int(int64_t units, int32_t scale) {
  fob::common::v1::Decimal d;
  d.set_units(units);
  d.set_scale(scale);
  return d;
}

VenuesLoop::VenuesLoop(const std::string& brokers)
    : brokers_(brokers),
      producer_({.brokers=brokers, .client_id="venues"}),
      consumer_({.brokers=brokers, .group_id="venues", .client_id="venues", .enable_auto_commit=false}) {}

void VenuesLoop::start() {
  running_.store(true);
  consumer_.subscribe({"execution.intents"});
  t_md_ = std::thread([this] { md_publish_loop(); });
  t_exec_ = std::thread([this] { exec_consume_loop(); });
}

void VenuesLoop::stop() {
  running_.store(false);
  if (t_md_.joinable()) t_md_.join();
  if (t_exec_.joinable()) t_exec_.join();
}

void VenuesLoop::md_publish_loop() {
  using namespace std::chrono;

  // Simple synthetic ticker stream for BTC/USDT around 100.00
  int64_t price_units = 10000; // 100.00 with scale=2

  while (running_.load()) {
    fob::marketdata::v1::MarketDataRaw evt;
    auto* meta = evt.mutable_meta();
    meta->set_event_id(cex::common::uuid_v4());
    *meta->mutable_ts_event() = cex::common::now_ts();
    meta->set_source("venues");
    meta->set_correlation_id(cex::common::uuid_v4());
    meta->set_partition_key("binance|BTC/USDT");

    auto* t = evt.mutable_ticker();
    t->set_venue("binance");
    t->mutable_instrument()->set_symbol("BTC/USDT");
    t->mutable_instrument()->set_base("BTC");
    t->mutable_instrument()->set_quote("USDT");
    *t->mutable_timestamp() = cex::common::now_ts();

    // small oscillation
    price_units += (price_units % 2 == 0 ? 1 : -1);

    *t->mutable_bid() = dec_from_int(price_units - 1, 2);
    *t->mutable_ask() = dec_from_int(price_units + 1, 2);
    *t->mutable_last() = dec_from_int(price_units, 2);
    *t->mutable_spread() = dec_from_int(2, 2);

    producer_.produce("marketdata.raw", meta->partition_key(), cex::common::to_bytes(evt));

    std::this_thread::sleep_for(milliseconds(500));
  }
}

void VenuesLoop::exec_consume_loop() {
  while (running_.load()) {
    bool ok = consumer_.poll_once(500, [this](const std::string& topic,
                                            const std::string& key,
                                            const std::string& payload) {
      (void)topic; (void)key;
      fob::execution::v1::ExecutionIntent intent;
      if (!cex::common::from_bytes(payload, intent)) {
        cex::common::log_json("ERROR", "Failed to parse ExecutionIntent");
        return;
      }

      // Simulate immediate fill at limit_price if set, else at 100.00.
      fob::execution::v1::ExecutionReport rep;
      auto* meta = rep.mutable_meta();
      meta->set_event_id(cex::common::uuid_v4());
      *meta->mutable_ts_event() = cex::common::now_ts();
      meta->set_source("venues");
      meta->set_correlation_id(intent.meta().correlation_id());
      meta->set_partition_key(intent.intent_id());

      rep.set_report_id(cex::common::uuid_v4());
      rep.set_intent_id(intent.intent_id());
      rep.set_venue(intent.venue());
      *rep.mutable_instrument() = intent.instrument();
      rep.set_venue_symbol(intent.venue_symbol());
      rep.set_venue_order_id("VENUE-" + intent.intent_id());
      rep.set_client_order_id(intent.client_order_id());

      rep.set_status(fob::common::v1::ORDER_STATUS_FILLED);
      *rep.mutable_filled_qty() = intent.target_qty();
      rep.mutable_remaining_qty()->set_units(0);
      rep.mutable_remaining_qty()->set_scale(intent.target_qty().scale());

      // avg price
      if (intent.has_limit_price() && intent.limit_price().units() != 0) {
        *rep.mutable_average_price() = intent.limit_price();
      } else {
        rep.mutable_average_price()->set_units(10000);
        rep.mutable_average_price()->set_scale(2);
      }

      producer_.produce("execution.reports", intent.intent_id(), cex::common::to_bytes(rep));

      cex::common::log_json("INFO", "Produced execution.reports",
                            {{"intent_id", intent.intent_id()},
                             {"venue", intent.venue()}});
    });

    if (!ok) break;
  }
}

}  // namespace cex::venues::app
