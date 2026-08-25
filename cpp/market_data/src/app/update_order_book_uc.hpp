#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "fob/marketdata/v1/marketdata_raw.pb.h"
#include "fob/matching/v1/batch.grpc.pb.h"
#include "fob/venue/v1/venue.pb.h"

#include "domain/ports/i_order_book_publisher.hpp"
#include "domain/ports/i_order_book_storage.hpp"

namespace cex::market_data::app {

class UpdateOrderBookUseCase {
 public:
  explicit UpdateOrderBookUseCase(domain::IOrderBookStorage* storage,
                                  domain::IOrderBookPublisher* publisher);

  void OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt);
  void OnBatchResult(const fob::matching::v1::BatchResult& result);
  void OnVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot);

 private:
  static std::string VenueSymbolKey(const fob::venue::v1::VenueSnapshot& snapshot);
  static bool IsSnapshotAggregatable(const fob::venue::v1::VenueSnapshot& snapshot);
  void RebuildAggregatedVenueBook(const std::string& symbol);
  void PublishBBOIfChanged();  // New helper method
  
  std::optional<domain::OrderBook> book_;
  domain::IOrderBookStorage* storage_;
  domain::IOrderBookPublisher* publisher_;
  uint64_t venue_aggregate_nonce_{0};
  std::unordered_map<std::string, fob::venue::v1::VenueSnapshot> latest_venue_snapshots_;
  
  // Cache last published BBO to avoid spamming
  std::optional<domain::BBO> last_published_bbo_;
};

}  // namespace cex::market_data::app
