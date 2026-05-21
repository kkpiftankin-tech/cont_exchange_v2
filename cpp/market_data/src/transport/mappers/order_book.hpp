#pragma once

#include "fob/marketdata/v1/marketdata_raw.grpc.pb.h"
#include "fob/matching/v1/batch.grpc.pb.h"
#include "domain/entities/order_book.hpp"

namespace cex::market_data::transport::mappers {

domain::OrderBook FromProto(const fob::marketdata::v1::MarketDataRaw& data);

domain::OrderBook FromProto(const fob::venue::v1::VenueSnapshot& snapshot, uint64_t nonce);

fob::marketdata::v1::OrderBookSnapshot ToProto(const domain::OrderBook& book);

std::vector<domain::FillEvent> ExtractFillEvents(const fob::matching::v1::BatchResult& result);

}  // namespace cex::market_data::transport::mappers
