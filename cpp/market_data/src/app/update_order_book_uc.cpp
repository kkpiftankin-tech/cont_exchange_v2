#include "update_order_book_uc.hpp"

#include <tuple>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"
#include "transport/mappers/order_book.hpp"

namespace cex::market_data::app {

UpdateOrderBookUseCase::UpdateOrderBookUseCase(domain::IOrderBookStorage* storage,
                                               domain::IOrderBookPublisher* publisher)
    : storage_(storage), publisher_(publisher) {}

std::string UpdateOrderBookUseCase::VenueSymbolKey(
    const fob::venue::v1::VenueSnapshot& snapshot) {
  return snapshot.venue_id() + "|" + snapshot.instrument().symbol();
}

bool UpdateOrderBookUseCase::IsSnapshotAggregatable(
    const fob::venue::v1::VenueSnapshot& snapshot) {
  const std::string status = snapshot.status();
  if (status == "empty" || status == "stale" || status == "disconnected" || status == "off") {
    return false;
  }
  return snapshot.bid_prices_size() > 0 && snapshot.ask_prices_size() > 0 &&
         snapshot.bid_quantities_size() == snapshot.bid_prices_size() &&
         snapshot.ask_quantities_size() == snapshot.ask_prices_size();
}

void UpdateOrderBookUseCase::RebuildAggregatedVenueBook(const std::string& symbol) {
  if (symbol.empty()) return;

  domain::OrderBook aggregated;
  aggregated.symbol = symbol;
  aggregated.source = domain::MarketSource::Unknown;
  aggregated.nonce = ++venue_aggregate_nonce_;

  bool has_any = false;
  int64_t max_seconds = 0;
  int32_t max_nanos = 0;

  auto merge_bid = [&](const cex::common::Decimal& price, const cex::common::Decimal& qty) {
    if (cex::common::Decimal::cmp(qty, cex::common::Decimal::zero()) <= 0) return;
    domain::PriceLevel probe{price, cex::common::Decimal::zero()};
    auto it = aggregated.bid_depth.find(probe);
    if (it == aggregated.bid_depth.end()) {
      aggregated.bid_depth.insert(domain::PriceLevel{price, qty});
      return;
    }
    const auto merged_qty = cex::common::Decimal::add(it->quantity, qty);
    aggregated.bid_depth.erase(it);
    aggregated.bid_depth.insert(domain::PriceLevel{price, merged_qty});
  };

  auto merge_ask = [&](const cex::common::Decimal& price, const cex::common::Decimal& qty) {
    if (cex::common::Decimal::cmp(qty, cex::common::Decimal::zero()) <= 0) return;
    domain::PriceLevel probe{price, cex::common::Decimal::zero()};
    auto it = aggregated.ask_depth.find(probe);
    if (it == aggregated.ask_depth.end()) {
      aggregated.ask_depth.insert(domain::PriceLevel{price, qty});
      return;
    }
    const auto merged_qty = cex::common::Decimal::add(it->quantity, qty);
    aggregated.ask_depth.erase(it);
    aggregated.ask_depth.insert(domain::PriceLevel{price, merged_qty});
  };

  for (const auto& [key, snapshot] : latest_venue_snapshots_) {
    (void)key;
    if (snapshot.instrument().symbol() != symbol || !IsSnapshotAggregatable(snapshot)) continue;
    has_any = true;

    if (snapshot.has_timestamp()) {
      const auto sec = snapshot.timestamp().seconds();
      const auto ns = snapshot.timestamp().nanos();
      if (std::tie(sec, ns) > std::tie(max_seconds, max_nanos)) {
        max_seconds = sec;
        max_nanos = ns;
      }
    }

    for (int i = 0; i < snapshot.bid_prices_size(); ++i) {
      merge_bid(cex::common::Decimal::from_proto(snapshot.bid_prices(i)),
                cex::common::Decimal::from_proto(snapshot.bid_quantities(i)));
    }
    for (int i = 0; i < snapshot.ask_prices_size(); ++i) {
      merge_ask(cex::common::Decimal::from_proto(snapshot.ask_prices(i)),
                cex::common::Decimal::from_proto(snapshot.ask_quantities(i)));
    }
  }

  if (!has_any || aggregated.bid_depth.empty() || aggregated.ask_depth.empty()) {
    return;
  }

  aggregated.timestamp = domain::Timestamp{
      std::chrono::seconds(max_seconds) + std::chrono::nanoseconds(max_nanos)};
  book_ = std::move(aggregated);
}

void UpdateOrderBookUseCase::PublishBBOIfChanged() {
  if (!book_.has_value()) return;

  domain::BBO current_bbo = book_->GetBBO();

  // Only publish if BBO changed or first time
  if (!last_published_bbo_.has_value() ||
      last_published_bbo_->bid_price != current_bbo.bid_price ||
      last_published_bbo_->ask_price != current_bbo.ask_price ||
      last_published_bbo_->bid_volume != current_bbo.bid_volume ||
      last_published_bbo_->ask_volume != current_bbo.ask_volume) {
    publisher_->PublishBBO(current_bbo, book_->symbol, book_->nonce, book_->timestamp);
    last_published_bbo_ = current_bbo;
  }
}

void UpdateOrderBookUseCase::OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt) {
  book_ = transport::mappers::FromProto(evt);
  storage_->Store(*book_);
  publisher_->Publish(*book_);
  PublishBBOIfChanged();
}

void UpdateOrderBookUseCase::OnBatchResult(const fob::matching::v1::BatchResult& evt) {
  if (!book_.has_value()) {
    cex::common::log_json("WARN", "batch.outputs arrived before first order book snapshot",
                          {{"batch_id", evt.batch_id()}});
    return;
  }

  for (const auto& event : transport::mappers::ExtractFillEvents(evt)) {
    book_.value().Update(event);
  }
  storage_->Store(*book_);
  publisher_->Publish(*book_);
  PublishBBOIfChanged();
}

void UpdateOrderBookUseCase::OnVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) {
  latest_venue_snapshots_[VenueSymbolKey(snapshot)] = snapshot;
  RebuildAggregatedVenueBook(snapshot.instrument().symbol());
  if (!book_.has_value()) return;
  storage_->Store(*book_);
  publisher_->Publish(*book_);
  PublishBBOIfChanged();
}

}  // namespace cex::market_data::app
